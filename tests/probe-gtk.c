/*
 * probe-gtk: emulates what the Unity Editor does when panning reaches a screen
 * edge.  It asks GDK to warp the pointer to an absolute screen position.
 *
 *   GDK_BACKEND=x11 ./probe-gtk 0 500
 *
 * Without the shim, the real pointer does not move (Wayland has no warp).
 * With libunity-cursor-wrap.so preloaded and UNITY_CURSOR_WRAP_FORCE=1, the
 * pointer is moved through Hyprland IPC.
 */

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <stdio.h>
#include <stdlib.h>

static int main_loop_done;

static gboolean warp(gpointer data)
{
    (void)data;
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = gdk_seat_get_pointer(seat);

    int x = 0, y = 0;
    gdk_device_get_position(pointer, NULL, &x, &y);
    printf("display: %s (x11=%d)\n", gdk_display_get_name(display),
           GDK_IS_X11_DISPLAY(display) ? 1 : 0);
    printf("before: %d,%d\n", x, y);
    fflush(stdout);

    int target_x = 0, target_y = 0;
    if (sscanf(g_getenv("PROBE_TARGET") ? g_getenv("PROBE_TARGET") : "", "%d,%d",
               &target_x, &target_y) != 2) {
        target_x = 0;
        target_y = y;
    }

    gdk_device_warp(pointer, gdk_screen_get_default(), target_x, target_y);
    printf("warp requested: %d,%d\n", target_x, target_y);
    fflush(stdout);
    return G_SOURCE_REMOVE;
}

static gboolean report(gpointer data)
{
    (void)data;
    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = gdk_seat_get_pointer(seat);
    int x = 0, y = 0;
    gdk_device_get_position(pointer, NULL, &x, &y);
    printf("after: %d,%d\n", x, y);
    fflush(stdout);
    fflush(stdout);
    main_loop_done = 1;
    gtk_main_quit();
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    gtk_init(NULL, NULL);
    g_timeout_add(300, warp, NULL);
    g_timeout_add(900, report, NULL);
    gtk_main();
    return main_loop_done ? 0 : 1;
}
