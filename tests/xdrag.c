/*
 * xdrag: drive a drag through XTest so the shim can be tested end to end
 * without a human at the mouse.
 *
 *   ./xdrag <button> <x1> <y1> <x2> <y2> [steps]
 *
 * XTest moves the X server's pointer, not the Wayland cursor, which is exactly
 * what an X11 application observes while the compositor keeps the real cursor
 * where it was.  That makes it a faithful stand-in for "drag into the edge".
 */

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static void move(Display *dpy, int x, int y)
{
    XTestFakeMotionEvent(dpy, -1, x, y, CurrentTime);
    XFlush(dpy);
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s <button> <x1> <y1> <x2> <y2> [steps]\n", argv[0]);
        return 2;
    }

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "cannot open display\n");
        return 1;
    }

    int button = atoi(argv[1]);
    int x1 = atoi(argv[2]);
    int y1 = atoi(argv[3]);
    int x2 = atoi(argv[4]);
    int y2 = atoi(argv[5]);
    int steps = argc > 6 ? atoi(argv[6]) : 8;
    if (steps < 1)
        steps = 1;

    move(dpy, x1, y1);
    usleep(150000);

    if (button > 0) {
        XTestFakeButtonEvent(dpy, button, True, CurrentTime);
        XFlush(dpy);
        usleep(150000);
    }

    for (int i = 1; i <= steps; i++) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        move(dpy, x, y);
        usleep(120000);
    }

    /* Real mice keep emitting motion events while the pointer is pinned at
     * the screen edge; Unity only reacts to those.  Jiggle by one pixel so
     * every sample is a new event. */
    if (getenv("XDRAG_JIGGLE")) {
        for (int i = 0; i < 20; i++) {
            move(dpy, x2 + (i % 2 ? 1 : -1), y2 + (i % 3 ? 0 : 1));
            usleep(90000);
        }
    }

    usleep(200000);
    if (button > 0) {
        XTestFakeButtonEvent(dpy, button, False, CurrentTime);
        XFlush(dpy);
    }

    XCloseDisplay(dpy);
    return 0;
}
