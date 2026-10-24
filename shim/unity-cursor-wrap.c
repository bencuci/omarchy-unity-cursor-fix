/*
 * unity-cursor-wrap.c
 *
 * LD_PRELOAD shim for the Unity Editor on Wayland (Hyprland).
 *
 * Why this exists
 * ---------------
 * While dragging in the Scene view, Unity asks GTK/GDK to move the pointer to
 * the opposite edge of the screen when it reaches an edge:
 *
 *     gdk_device_warp() -> (GDK X11 backend) -> XIWarpPointer()
 *
 * On a real X11 session the X server moves the pointer, and Unity's own input
 * bookkeeping handles the resulting jump, so panning continues forever.
 *
 * Under XWayland, Wayland gives clients no way to set the pointer position, so
 * the request is silently dropped: the pointer stops at the edge and panning
 * stops.  External warpers (e.g. a compositor-side "wrap the cursor" helper)
 * move the pointer, but Unity sees a raw position delta of a whole screen width
 * and snaps the camera to the opposite side.
 *
 * This shim keeps Unity as the source of truth.  It intercepts the warp
 * requests Unity already makes and replays them through Hyprland's IPC socket,
 * which is allowed to move the real pointer:
 *
 *     gdk_device_warp() -> shim -> Hyprland IPC -> real pointer moves
 *
 * Unity observes the pointer at the exact coordinates it asked for, so its own
 * delta compensation does what it does on X11.
 *
 * Safety rails
 * ------------
 *   - only active inside a Unity Editor process
 *   - only for warps that land near an outer screen edge
 *   - only while a mouse button is held (i.e. during a drag)
 *   - disabled by the omarchy plugin through the state file
 *   - if Hyprland is unreachable, the original call is made instead
 *   - set UNITY_CURSOR_WRAP_DEBUG=1 to log to
 *     $XDG_STATE_HOME/omarchy/unity-cursor-wrap.log
 *   - set UNITY_CURSOR_WRAP_FORCE=1 to bypass the editor/drag/edge checks
 */

#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#define EDGE_MARGIN_DEFAULT 16
#define DEDUPE_MS 25

typedef void (*gdk_device_warp_fn)(void *device, void *screen, int x, int y);
typedef int (*xi_warp_fn)(Display *dpy, int deviceid, Window src_w, Window dest_w,
                          double src_x, double src_y, unsigned int src_width,
                          unsigned int src_height, double dest_x, double dest_y);
typedef int (*x_warp_fn)(Display *dpy, Window src_w, Window dest_w, int src_x,
                         int src_y, unsigned int src_width, unsigned int src_height,
                         int dest_x, int dest_y);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static gdk_device_warp_fn real_gdk_device_warp;
static xi_warp_fn real_xi_warp_pointer;
static x_warp_fn real_x_warp_pointer;

static Display *g_dpy;
static Window g_root;
static int g_width;
static int g_height;

static int g_init_done;
static long long g_last_warp_ms;
static int g_last_x = -1;
static int g_last_y = -1;

/* Virtual-pointer mode (Mode B).
 *
 * Unity pans from absolute pointer positions.  Wayland stops delivering motion
 * events once the pointer is pinned to a screen edge, so the drag stops.  We
 * warp at the edge and add an offset to every coordinate Unity sees, which
 * keeps its deltas continuous while the real pointer keeps looping. */
static int g_off_x;
static int g_off_y;
static int g_wrapped_guard;  /* set right after a wrap, cleared away from edge */
static long long g_last_native_warp;
static int g_virtual_mode = -1; /* lazily read from UNITY_CURSOR_WRAP_MODE */
static int g_edge_inset = 2;

/* ------------------------------------------------------------------ utils */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * dlsym() is interposed below, so resolving symbols through it from inside
 * this library would hand back our own wrappers.  Go through the real dlsym
 * with RTLD_NEXT (relative to this library), which skips us.
 */
static void *(*g_real_dlsym)(void *, const char *);

static void init_real_dlsym(void)
{
    if (g_real_dlsym)
        return;
    g_real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!g_real_dlsym)
        g_real_dlsym = (void *(*)(void *, const char *))dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
}

static void *next_symbol(const char *name)
{
    init_real_dlsym();
    if (!g_real_dlsym)
        return NULL;
    return g_real_dlsym(RTLD_NEXT, name);
}

static const char *state_home(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && *xdg)
        return xdg;
    return NULL;
}

static void log_path(char *buf, size_t len)
{
    const char *override = getenv("UNITY_CURSOR_WRAP_LOG");
    if (override && *override) {
        snprintf(buf, len, "%s", override);
        return;
    }
    const char *home = getenv("HOME");
    const char *state = state_home();
    if (state)
        snprintf(buf, len, "%s/omarchy/unity-cursor-wrap.log", state);
    else if (home)
        snprintf(buf, len, "%s/.local/state/omarchy/unity-cursor-wrap.log", home);
    else
        buf[0] = '\0';
}

static int debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("UNITY_CURSOR_WRAP_DEBUG");
        cached = (v && *v && strcmp(v, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static void logmsg(const char *fmt, ...)
{
    if (!debug_enabled())
        return;

    char path[512];
    log_path(path, sizeof path);
    if (!path[0])
        return;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;

    char line[1024];
    int n = snprintf(line, sizeof line, "[%lld] pid=%d ", now_ms(), (int)getpid());
    if (n < 0 || (size_t)n >= sizeof line) {
        close(fd);
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);

    size_t used = strlen(line);
    if (used + 1 < sizeof line) {
        line[used] = '\n';
        line[used + 1] = '\0';
        used++;
    }
    if (write(fd, line, used) < 0) {
        /* nothing useful to do */
    }
    close(fd);
}

static int is_unity_editor(void)
{
    static int cached = -1;
    if (cached >= 0)
        return cached;

    cached = 0;
    char link[512];
    ssize_t n = readlink("/proc/self/exe", link, sizeof link - 1);
    if (n > 0) {
        link[n] = '\0';
        const char *base = strrchr(link, '/');
        base = base ? base + 1 : link;
        if (strstr(base, "unityhub-unity-editor") || strcmp(base, "Unity") == 0) {
            /* Asset import workers share the binary but are headless. */
            char cmdline[512] = {0};
            int fd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t got = read(fd, cmdline, sizeof cmdline - 1);
                close(fd);
                if (got > 0)
                    for (ssize_t i = 0; i < got; i++)
                        if (cmdline[i] == '\0')
                            cmdline[i] = ' ';
            }
            if (!strstr(cmdline, "-batchMode"))
                cached = 1;
        }
    }
    if (getenv("UNITY_CURSOR_WRAP_FORCE"))
        cached = 1;
    return cached;
}

/*
 * The plugin toggles the feature by writing {"enabled": bool} to the state
 * file.  Cache the answer by mtime so the hot path costs one stat().
 */
static int feature_enabled(void)
{
    static long long last_check;
    static int cached = 1;
    static int initialised;

    long long now = now_ms();
    if (initialised && now - last_check < 500)
        return cached;
    last_check = now;
    initialised = 1;

    const char *home = getenv("HOME");
    const char *state = state_home();
    char path[512];

    if (state)
        snprintf(path, sizeof path, "%s/omarchy/unity-cursor-wrap.json", state);
    else if (home)
        snprintf(path, sizeof path, "%s/.local/state/omarchy/unity-cursor-wrap.json", home);
    else
        return cached;

    FILE *f = fopen(path, "re");
    if (!f) {
        cached = 1; /* no state file means "installed and on" */
        return cached;
    }
    char buf[512];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[got] = '\0';

    cached = 1;
    char *at = strstr(buf, "\"enabled\"");
    if (at) {
        at += strlen("\"enabled\"");
        while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
            at++;
        if (*at == ':') {
            at++;
            while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
                at++;
            if (strncmp(at, "false", 5) == 0)
                cached = 0;
        }
    }
    return cached;
}

static int active(void)
{
    const char *opt_out = getenv("UNITY_CURSOR_WRAP");
    if (opt_out && strcmp(opt_out, "0") == 0)
        return 0;
    return is_unity_editor() && feature_enabled();
}

/* ------------------------------------------------------------- X display */

static int ensure_display(void)
{
    if (g_init_done)
        return g_dpy != NULL;
    g_init_done = 1;

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        logmsg("no X display available");
        return 0;
    }
    int screen = DefaultScreen(g_dpy);
    g_root = RootWindow(g_dpy, screen);
    g_width = DisplayWidth(g_dpy, screen);
    g_height = DisplayHeight(g_dpy, screen);
    logmsg("connected to X: root=%dx%d", g_width, g_height);
    return 1;
}

static int button_down(void)
{
    if (getenv("UNITY_CURSOR_WRAP_FORCE"))
        return 1;
    if (!ensure_display())
        return 0;

    Window root_ret = 0, child_ret = 0;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(g_dpy, g_root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask))
        return 0;
    return (mask & (Button1Mask | Button2Mask | Button3Mask)) != 0;
}

static int near_edge(int x, int y)
{
    if (getenv("UNITY_CURSOR_WRAP_FORCE"))
        return 1;
    if (!ensure_display())
        return 0;

    int margin = EDGE_MARGIN_DEFAULT;
    const char *m = getenv("UNITY_CURSOR_WRAP_MARGIN");
    if (m && *m)
        margin = atoi(m);
    if (margin < 1)
        margin = 1;

    return x <= margin || y <= margin || x >= g_width - 1 - margin ||
           y >= g_height - 1 - margin;
}

/* ------------------------------------------------------- Hyprland client */

static int hypr_socket_path(char *buf, size_t len)
{
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime)
        return 0;

    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (sig && *sig) {
        snprintf(buf, len, "%s/hypr/%s/.socket.sock", runtime, sig);
        if (access(buf, F_OK) == 0)
            return 1;
    }

    /* Fall back to the single instance directory when the signature is absent
     * (for example when the editor was started outside a Hyprland session). */
    char base[512];
    if ((size_t)snprintf(base, sizeof base, "%s/hypr", runtime) >= sizeof base)
        return 0;
    DIR *dir = opendir(base);
    if (!dir)
        return 0;

    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        int written = snprintf(buf, len, "%s/%s/.socket.sock", base, entry->d_name);
        if (written > 0 && (size_t)written < len && access(buf, F_OK) == 0) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

static int hypr_move(int x, int y)
{
    char path[512];
    if (!hypr_socket_path(path, sizeof path)) {
        logmsg("hyprland socket not found");
        return 0;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        close(fd);
        logmsg("hyprland socket path too long");
        return 0;
    }
    strcpy(addr.sun_path, path);

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    int ok = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
        /* Hyprland 0.5x evaluates dispatchers as Lua; keep the legacy
         * dispatcher as a fallback for older releases. */
        char request[256];
        snprintf(request, sizeof request,
                 "eval hl.dispatch(hl.dsp.cursor.move({x=%d,y=%d}))", x, y);
        if (send(fd, request, strlen(request), 0) >= 0) {
            char reply[256];
            ssize_t got = recv(fd, reply, sizeof reply - 1, 0);
            if (got > 0) {
                reply[got] = '\0';
                if (strncmp(reply, "ok", 2) == 0) {
                    ok = 1;
                } else {
                    logmsg("lua move rejected: %s", reply);
                }
            }
        }
        if (!ok) {
            close(fd);
            fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) {
                char request[64];
                snprintf(request, sizeof request, "dispatch movecursor %d %d", x, y);
                if (send(fd, request, strlen(request), 0) >= 0) {
                    char reply[64];
                    ssize_t got = recv(fd, reply, sizeof reply - 1, 0);
                    if (got > 0) {
                        reply[got] = '\0';
                        if (strncmp(reply, "ok", 2) == 0)
                            ok = 1;
                        else
                            logmsg("dispatch rejected: %s", reply);
                    }
                }
            }
        }
    } else {
        logmsg("hyprland connect failed: %s", strerror(errno));
    }
    close(fd);
    return ok;
}

/* ------------------------------------------------------------- warp logic */

/*
 * Returns 1 when the shim took responsibility for the warp, 0 when the caller
 * should perform the original request.
 */
static int take_over_warp(int x, int y, const char *source)
{
    if (debug_enabled())
        logmsg("%s(%d,%d) seen, active=%d editor=%d enabled=%d", source, x, y,
               active(), is_unity_editor(), feature_enabled());
    if (!active())
        return 0;

    if (!near_edge(x, y)) {
        logmsg("%s target %d,%d is not on an edge, passing through", source, x, y);
        return 0;
    }
    if (!button_down()) {
        logmsg("%s target %d,%d without a held button, passing through", source, x, y);
        return 0;
    }

    long long now = now_ms();
    if (x == g_last_x && y == g_last_y && now - g_last_warp_ms < DEDUPE_MS) {
        logmsg("%s duplicate warp to %d,%d ignored", source, x, y);
        return 1;
    }

    if (!hypr_move(x, y)) {
        logmsg("%s warp to %d,%d could not be delivered", source, x, y);
        return 0;
    }

    g_last_x = x;
    g_last_y = y;
    g_last_warp_ms = now;
    g_last_native_warp = now;
    logmsg("%s wrapped to %d,%d", source, x, y);
    return 1;
}

/* ------------------------------------------------------- child processes */

extern char **environ;

static const char *shim_path(void)
{
    static char path[512];
    static int resolved;
    if (!resolved) {
        resolved = 1;
        Dl_info info;
        if (dladdr((void *)&shim_path, &info) && info.dli_fname)
            snprintf(path, sizeof path, "%s", info.dli_fname);
    }
    return path[0] ? path : NULL;
}

static int is_editor_path(const char *path)
{
    if (!path || !*path)
        return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strcmp(base, "Unity") == 0)
        return 1;
    return strstr(base, "unityhub-unity-editor") != NULL;
}

static int value_mentions_shim(const char *value)
{
    const char *shim = shim_path();
    return shim && value && strstr(value, shim) != NULL;
}

/*
 * Unity Hub is an Electron app: it may sanitise its own environment before
 * spawning the editor, and Electron re-executes itself during startup.  So:
 *
 *   - never take LD_PRELOAD *away* (a re-exec of the Hub must keep the shim,
 *     otherwise editors spawned from the relaunched Hub lose it again)
 *   - make sure a Unity Editor child always receives the shim
 *
 * Returns NULL when the original environment can be used as-is.
 */
static char **adjust_env(char *const envp[], int want_shim)
{
    if (!want_shim)
        return NULL;

    const char *shim = shim_path();
    if (!shim)
        return NULL;

    char *const *source = envp ? envp : environ;
    size_t count = 0;
    while (source[count])
        count++;

    const char *current = NULL;
    size_t current_index = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(source[i], "LD_PRELOAD=", 11) == 0) {
            current = source[i] + 11;
            current_index = i;
            break;
        }
    }

    if (value_mentions_shim(current))
        return NULL;

    char composed[2560];
    snprintf(composed, sizeof composed, "%s%s%s", shim, (current && *current) ? ":" : "",
             current ? current : "");

    char **result = calloc(count + 2, sizeof(char *));
    if (!result)
        return NULL;

    size_t out = 0;
    for (size_t i = 0; i < count; i++) {
        if (current && i == current_index)
            continue;
        result[out++] = source[i];
    }
    size_t len = strlen("LD_PRELOAD=") + strlen(composed) + 1;
    char *entry = malloc(len);
    if (!entry) {
        free(result);
        return NULL;
    }
    snprintf(entry, len, "LD_PRELOAD=%s", composed);
    result[out++] = entry;
    result[out] = NULL;
    return result;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    static int (*real)(const char *, char *const[], char *const[]);
    if (!real)
        real = (int (*)(const char *, char *const[], char *const[]))next_symbol("execve");
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    char **env = adjust_env(envp, is_editor_path(path));
    return real(path, argv, env ? (char *const *)env : envp);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    static int (*real)(const char *, char *const[], char *const[]);
    if (!real)
        real = (int (*)(const char *, char *const[], char *const[]))next_symbol("execvpe");
    if (!real) {
        static int (*fallback)(const char *, char *const[]);
        if (!fallback)
            fallback = (int (*)(const char *, char *const[]))next_symbol("execvp");
        if (!fallback) {
            errno = ENOSYS;
            return -1;
        }
        return fallback(file, argv);
    }
    char **env = adjust_env(environ, is_editor_path(file));
    return real(file, argv, env ? (char *const *)env : environ);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    static int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                       const posix_spawnattr_t *, char *const[], char *const[]);
    if (!real)
        real = (int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *, char *const[], char *const[]))
            next_symbol("posix_spawn");
    if (!real)
        return ENOSYS;
    char **env = adjust_env(envp, is_editor_path(path));
    return real(pid, path, actions, attr, argv, env ? (char *const *)env : envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
    static int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                       const posix_spawnattr_t *, char *const[], char *const[]);
    if (!real)
        real = (int (*)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *, char *const[], char *const[]))
            next_symbol("posix_spawnp");
    if (!real)
        return ENOSYS;
    char **env = adjust_env(envp, is_editor_path(file));
    return real(pid, file, actions, attr, argv, env ? (char *const *)env : envp);
}

/* --------------------------------------------------- pointer diagnostics */

static int (*real_x_query_pointer)(Display *, Window, Window *, Window *, int *, int *,
                                   int *, int *, unsigned int *);

static int virtual_mode(void)
{
    if (g_virtual_mode < 0) {
        const char *mode = getenv("UNITY_CURSOR_WRAP_MODE");
        g_virtual_mode = (mode && strcmp(mode, "native") == 0) ? 0 : 1;
        const char *inset = getenv("UNITY_CURSOR_WRAP_INSET");
        if (inset && *inset) {
            int value = atoi(inset);
            if (value >= 1 && value <= 64)
                g_edge_inset = value;
        }
    }
    return g_virtual_mode;
}

static int event_buttons(XIDeviceEvent *event)
{
    if (!event->buttons.mask_len || !event->buttons.mask)
        return 0;
    return event->buttons.mask[0] & 0x07; /* buttons 1..3 */
}

static void reset_offsets(void)
{
    if (g_off_x || g_off_y)
        logmsg("virtual offsets reset (%d,%d)", g_off_x, g_off_y);
    g_off_x = 0;
    g_off_y = 0;
    g_wrapped_guard = 0;
}

/*
 * Called with an event whose coordinates have not been adjusted yet.  Decides
 * whether this sample is pushing against an outer edge during a drag, warps
 * the real pointer to the opposite edge and updates the offsets so the
 * coordinates handed to the application stay continuous.
 */
static void maybe_virtual_wrap(XIDeviceEvent *event)
{
    if (event->evtype != XI_Motion)
        return; /* never warp just because a click landed near an edge */
    if (!virtual_mode() || !active())
        return;
    if (now_ms() - g_last_native_warp < 200)
        return;
    if (!ensure_display())
        return;

    int x = (int)event->root_x;
    int y = (int)event->root_y;
    int target_x = x;
    int target_y = y;
    const char *edge = NULL;

    int left = 0, top = 0, right = g_width - 1, bottom = g_height - 1;

    if (g_wrapped_guard) {
        /* Wait until the pointer has moved away from the landing edge. */
        if (x > left + 4 && x < right - 4 && y > top + 4 && y < bottom - 4)
            g_wrapped_guard = 0;
        return;
    }

    if (x <= left) {
        edge = "left";
        target_x = right - g_edge_inset;
    } else if (x >= right) {
        edge = "right";
        target_x = left + g_edge_inset;
    } else if (y <= top) {
        edge = "top";
        target_y = bottom - g_edge_inset;
    } else if (y >= bottom) {
        edge = "bottom";
        target_y = top + g_edge_inset;
    } else {
        return;
    }

    /* XInput2 button state turned out to be unreliable across XWayland and
     * GDK, so ask the server directly.  This only happens within a pixel of an
     * outer edge, never in the normal motion path. */
    if (!button_down())
        return;

    int virtual_x = x + g_off_x;
    int virtual_y = y + g_off_y;

    if (!hypr_move(target_x, target_y)) {
        logmsg("virtual wrap on %s failed to move pointer", edge);
        return;
    }

    g_off_x = virtual_x - target_x;
    g_off_y = virtual_y - target_y;
    g_wrapped_guard = 1;
    logmsg("virtual wrap %s: real %d,%d -> %d,%d offset now %d,%d", edge, x, y, target_x,
           target_y, g_off_x, g_off_y);
}
static Bool (*real_x_get_event_data)(Display *, XGenericEventCookie *);

int XQueryPointer(Display *dpy, Window w, Window *root_ret, Window *child_ret, int *root_x,
                  int *root_y, int *win_x, int *win_y, unsigned int *mask)
{
    if (!real_x_query_pointer)
        real_x_query_pointer =
            (int (*)(Display *, Window, Window *, Window *, int *, int *, int *, int *,
                     unsigned int *))next_symbol("XQueryPointer");
    if (!real_x_query_pointer)
        return 0;

    int result = real_x_query_pointer(dpy, w, root_ret, child_ret, root_x, root_y, win_x,
                                      win_y, mask);

    if (debug_enabled() && result) {
        static long long last_log;
        long long now = now_ms();
        if (mask && (*mask & (Button1Mask | Button2Mask | Button3Mask)) && now - last_log > 100) {
            last_log = now;
            logmsg("XQueryPointer %.0f,%.0f mask=0x%x", root_x ? (double)*root_x : -1.0,
                   root_y ? (double)*root_y : -1.0, *mask);
        }
    }
    return result;
}

Bool XGetEventData(Display *dpy, XGenericEventCookie *cookie)
{
    if (!real_x_get_event_data)
        real_x_get_event_data = (Bool (*)(Display *, XGenericEventCookie *))
            next_symbol("XGetEventData");
    if (!real_x_get_event_data)
        return False;

    Bool result = real_x_get_event_data(dpy, cookie);
    if (!result || !cookie || cookie->type != GenericEvent ||
        cookie->extension != 131 /* XInputExtension */ || !cookie->data)
        return result;

    XIDeviceEvent *event = (XIDeviceEvent *)cookie->data;
    if (event->evtype != XI_Motion && event->evtype != XI_ButtonPress &&
        event->evtype != XI_ButtonRelease)
        return result;

    int buttons = event_buttons(event);

    if (debug_enabled()) {
        static long long last_log;
        long long now = now_ms();
        int near_edge = event->root_x <= 2 || event->root_y <= 2 ||
                        event->root_x >= g_width - 3 || event->root_y >= g_height - 3;
        if (event->evtype != XI_Motion || (near_edge && now - last_log > 100)) {
            last_log = now;
            logmsg("XI event=%d root=%.0f,%.0f event=%.0f,%.0f buttons=0x%x", event->evtype,
                   event->root_x, event->root_y, event->event_x, event->event_y, buttons);
        }
    }

    int release_ends_drag = 0;

    pthread_mutex_lock(&g_lock);

    maybe_virtual_wrap(event);

    /* Coordinates handed to the application are shifted by the accumulated
     * offset, so a wrap is invisible to its delta math. */
    if (g_off_x || g_off_y) {
        event->root_x += g_off_x;
        event->root_y += g_off_y;
        event->event_x += g_off_x;
        event->event_y += g_off_y;
    }

    if (event->evtype == XI_ButtonRelease)
        release_ends_drag = 1;
    pthread_mutex_unlock(&g_lock);

    if (release_ends_drag) {
        /* Delivered with the offset (so the release delta stays sane), then
         * the next sample snaps back to real coordinates while nothing is
         * being dragged. */
        pthread_mutex_lock(&g_lock);
        reset_offsets();
        pthread_mutex_unlock(&g_lock);
    }

    return result;
}

static int (*real_x_grab_pointer)(Display *, Window, Bool, unsigned int, int, int, Window,
                                  Cursor, Time);

int XGrabPointer(Display *dpy, Window grab_window, Bool owner_events, unsigned int event_mask,
                 int pointer_mode, int keyboard_mode, Window confine_to, Cursor cursor, Time time)
{
    if (!real_x_grab_pointer)
        real_x_grab_pointer = (int (*)(Display *, Window, Bool, unsigned int, int, int, Window,
                                       Cursor, Time))next_symbol("XGrabPointer");
    if (debug_enabled())
        logmsg("XGrabPointer window=0x%lx confine=0x%lx mask=0x%x owner=%d", grab_window,
               confine_to, event_mask, owner_events);
    if (real_x_grab_pointer)
        return real_x_grab_pointer(dpy, grab_window, owner_events, event_mask, pointer_mode,
                                   keyboard_mode, confine_to, cursor, time);
    return 0;
}

/* ------------------------------------------------------------------ hooks */

void gdk_device_warp(void *device, void *screen, int x, int y)
{
    if (!real_gdk_device_warp)
        real_gdk_device_warp = (gdk_device_warp_fn)next_symbol("gdk_device_warp");

    pthread_mutex_lock(&g_lock);
    int handled = take_over_warp(x, y, "gdk_device_warp");
    pthread_mutex_unlock(&g_lock);

    if (!handled && real_gdk_device_warp)
        real_gdk_device_warp(device, screen, x, y);
}

int XIWarpPointer(Display *dpy, int deviceid, Window src_w, Window dest_w, double src_x,
                  double src_y, unsigned int src_width, unsigned int src_height,
                  double dest_x, double dest_y)
{
    if (!real_xi_warp_pointer)
        real_xi_warp_pointer = (xi_warp_fn)next_symbol("XIWarpPointer");

    int x = (int)dest_x;
    int y = (int)dest_y;
    int translated = 0;

    /* XIWarpPointer coordinates are relative to dest_w when one is given. */
    if (dest_w != None && ensure_display()) {
        Window child = 0;
        int rx = 0, ry = 0;
        if (XTranslateCoordinates(dpy, dest_w, g_root, x, y, &rx, &ry, &child)) {
            x = rx;
            y = ry;
            translated = 1;
        }
    }

    pthread_mutex_lock(&g_lock);
    int handled = 0;
    if (translated || dest_w == None)
        handled = take_over_warp(x, y, "XIWarpPointer");
    pthread_mutex_unlock(&g_lock);

    if (handled)
        return 1;
    if (real_xi_warp_pointer)
        return real_xi_warp_pointer(dpy, deviceid, src_w, dest_w, src_x, src_y, src_width,
                                    src_height, dest_x, dest_y);
    return 0;
}

int XWarpPointer(Display *dpy, Window src_w, Window dest_w, int src_x, int src_y,
                 unsigned int src_width, unsigned int src_height, int dest_x, int dest_y)
{
    if (!real_x_warp_pointer)
        real_x_warp_pointer = (x_warp_fn)next_symbol("XWarpPointer");

    int x = dest_x;
    int y = dest_y;
    int translated = 0;

    if (dest_w != None && ensure_display()) {
        Window child = 0;
        int rx = 0, ry = 0;
        if (XTranslateCoordinates(dpy, dest_w, g_root, x, y, &rx, &ry, &child)) {
            x = rx;
            y = ry;
            translated = 1;
        }
    }

    pthread_mutex_lock(&g_lock);
    int handled = 0;
    if (translated || dest_w == None)
        handled = take_over_warp(x, y, "XWarpPointer");
    pthread_mutex_unlock(&g_lock);

    if (handled)
        return 1;
    if (real_x_warp_pointer)
        return real_x_warp_pointer(dpy, src_w, dest_w, src_x, src_y, src_width, src_height,
                                   dest_x, dest_y);
    return 0;
}

/*
 * Unity resolves some X11 entry points itself with dlsym() (its embedded SDL
 * layer), which would bypass normal symbol interposition.  Hand back our
 * wrappers for the warp functions and forward every other lookup untouched.
 */
void *dlsym(void *handle, const char *name)
{
    init_real_dlsym();
    if (!g_real_dlsym)
        return NULL;

    /* Only the Unity Editor needs the whitelist; everywhere else (Unity Hub,
     * Electron helpers, ...) behave exactly as before. */
    if (is_unity_editor()) {
        if (debug_enabled() && (name[0] == 'X' || strncmp(name, "gdk_", 4) == 0))
            logmsg("dlsym %s", name);
        if (strcmp(name, "gdk_device_warp") == 0)
            return (void *)gdk_device_warp;
        if (strcmp(name, "XIWarpPointer") == 0)
            return (void *)XIWarpPointer;
        if (strcmp(name, "XWarpPointer") == 0)
            return (void *)XWarpPointer;
    }
    return g_real_dlsym(handle, name);
}
