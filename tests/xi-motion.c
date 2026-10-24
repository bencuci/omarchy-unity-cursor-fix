/*
 * xi-motion: print the raw XInput2 motion state so the shim can use the right
 * field for "is a mouse button held".
 *
 *   ./xi-motion   (then drag with tests/vmouse in another shell)
 */

#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;

    int opcode, event, error;
    if (!XQueryExtension(dpy, "XInputExtension", &opcode, &event, &error))
        return 1;

    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    memset(mask, 0, sizeof mask);
    XISetMask(mask, XI_Motion);
    XISetMask(mask, XI_ButtonPress);
    XISetMask(mask, XI_ButtonRelease);

    XIEventMask evmask = { .deviceid = XIAllMasterDevices, .mask_len = sizeof mask, .mask = mask };
    XISelectEvents(dpy, DefaultRootWindow(dpy), &evmask, 1);
    XFlush(dpy);

    for (;;) {
        XEvent xev;
        XNextEvent(dpy, &xev);
        if (xev.type != GenericEvent || xev.xcookie.extension != opcode)
            continue;
        if (!XGetEventData(dpy, &xev.xcookie))
            continue;
        XIDeviceEvent *e = (XIDeviceEvent *)xev.xcookie.data;
        if (e->evtype == XI_Motion || e->evtype == XI_ButtonPress ||
            e->evtype == XI_ButtonRelease) {
            unsigned int first = 0;
            if (e->buttons.mask_len > 0 && e->buttons.mask)
                first = e->buttons.mask[0];
            printf("evtype=%d root=%.0f,%.0f mask_len=%d mask0=0x%x mods.base=0x%x\n",
                   e->evtype, e->root_x, e->root_y, e->buttons.mask_len, first,
                   e->mods.base);
            fflush(stdout);
        }
        XFreeEventData(dpy, &xev.xcookie);
    }
}
