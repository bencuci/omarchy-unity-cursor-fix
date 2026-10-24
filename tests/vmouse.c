/*
 * vmouse: drive the real pointer through /dev/uinput so the compositor sees a
 * genuine mouse (not XTest emulation).  Used to test what Unity actually does
 * when a physical mouse keeps pushing against a screen edge.
 *
 *   ./vmouse drag <left|middle|right> <dx> <dy> [steps] [delay_ms] [tail_ms]
 *
 * The whole gesture happens inside one process because a uinput device that is
 * destroyed mid-press would lift the button.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fd = -1;

static void emit(unsigned type, unsigned code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) {
        perror("write");
        exit(1);
    }
}

static void sync_now(void)
{
    emit(EV_SYN, SYN_REPORT, 0);
}

static void move_by(int dx, int dy)
{
    if (dx)
        emit(EV_REL, REL_X, dx);
    if (dy)
        emit(EV_REL, REL_Y, dy);
    sync_now();
}

static int button_code(const char *name)
{
    if (strcmp(name, "left") == 0)
        return BTN_LEFT;
    if (strcmp(name, "middle") == 0)
        return BTN_MIDDLE;
    if (strcmp(name, "right") == 0)
        return BTN_RIGHT;
    if (strcmp(name, "none") == 0)
        return 0;
    return -1;
}

static void create_device(void)
{
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("/dev/uinput");
        exit(1);
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
    ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    ioctl(fd, UI_SET_RELBIT, REL_X);
    ioctl(fd, UI_SET_RELBIT, REL_Y);

    struct uinput_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;
    snprintf(setup.name, sizeof setup.name, "unity-cursor-wrap test mouse");

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        perror("UI_DEV_SETUP");
        exit(1);
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
                "usage: %s drag <left|middle|right> <dx> <dy> [steps] [delay_ms] [tail_ms]\n",
                argv[0]);
        return 2;
    }

    int button = button_code(argv[2]);
    if (button < 0) {
        fprintf(stderr, "unknown button: %s\n", argv[2]);
        return 2;
    }

    int dx = atoi(argv[3]);
    int dy = atoi(argv[4]);
    int steps = argc > 5 ? atoi(argv[5]) : 20;
    int delay_ms = argc > 6 ? atoi(argv[6]) : 40;
    int tail_ms = argc > 7 ? atoi(argv[7]) : 400;
    if (steps < 1)
        steps = 1;

    create_device();
    usleep(1500000); /* let the compositor pick the device up */

    if (button > 0) {
        emit(EV_KEY, button, 1);
        sync_now();
        usleep(120000);
    }

    int moved_x = 0, moved_y = 0;
    for (int i = 1; i <= steps; i++) {
        int want_x = dx * i / steps;
        int want_y = dy * i / steps;
        move_by(want_x - moved_x, want_y - moved_y);
        moved_x = want_x;
        moved_y = want_y;
        usleep(delay_ms * 1000);
    }

    usleep(tail_ms * 1000);
    if (button > 0) {
        emit(EV_KEY, button, 0);
        sync_now();
        usleep(120000);
    }

    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
