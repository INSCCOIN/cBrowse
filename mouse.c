#include "mouse.h"
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define NDEV 8

static int fd[NDEV];
static int nfd, mx, my, maxx = 480, maxy = 320;
static int abs_min_x, abs_max_x = 4095, abs_min_y, abs_max_y = 4095;
static int have_abs, last_touch;

static int bit_set(const unsigned long *b, int n)
{
    return !!(b[n / (8 * sizeof(long))] & (1UL << (n % (8 * sizeof(long)))));
}

int mouse_open(int max_x, int max_y)
{
    int i;
    maxx = max_x > 1 ? max_x : 480;
    maxy = max_y > 1 ? max_y : 320;
    mx = maxx / 2;
    my = maxy / 2;
    nfd = 0;
    for (i = 0; i < 32 && nfd < NDEV; i++) {
        char path[64];
        unsigned long ev[8] = {0}, rel[8] = {0}, key[8] = {0}, absb[8] = {0};
        int d;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        d = open(path, O_RDONLY | O_NONBLOCK);
        if (d < 0)
            continue;
        if (ioctl(d, EVIOCGBIT(0, sizeof ev), ev) < 0) {
            close(d);
            continue;
        }
        ioctl(d, EVIOCGBIT(EV_REL, sizeof rel), rel);
        ioctl(d, EVIOCGBIT(EV_KEY, sizeof key), key);
        ioctl(d, EVIOCGBIT(EV_ABS, sizeof absb), absb);
        if (bit_set(rel, REL_X) || bit_set(absb, ABS_X) || bit_set(key, BTN_LEFT) ||
            bit_set(key, BTN_TOUCH)) {
            if (bit_set(absb, ABS_X)) {
                struct input_absinfo ax, ay;
                have_abs = 1;
                if (ioctl(d, EVIOCGABS(ABS_X), &ax) == 0) {
                    abs_min_x = ax.minimum;
                    abs_max_x = ax.maximum > ax.minimum ? ax.maximum : ax.minimum + 1;
                }
                if (ioctl(d, EVIOCGABS(ABS_Y), &ay) == 0) {
                    abs_min_y = ay.minimum;
                    abs_max_y = ay.maximum > ay.minimum ? ay.maximum : ay.minimum + 1;
                }
            }
            fd[nfd++] = d;
        } else
            close(d);
    }
    return nfd;
}

void mouse_close(void)
{
    int i;
    for (i = 0; i < nfd; i++)
        close(fd[i]);
    nfd = 0;
}

void mouse_add_fds(fd_set *rf)
{
    int i;
    for (i = 0; i < nfd; i++)
        FD_SET(fd[i], rf);
}

int mouse_max_fd(void)
{
    int i, m = -1;
    for (i = 0; i < nfd; i++)
        if (fd[i] > m)
            m = fd[i];
    return m;
}

int mouse_poll(int *x, int *y, int *click)
{
    int i, got = 0;
    *click = 0;
    for (i = 0; i < nfd; i++) {
        struct input_event e;
        while (read(fd[i], &e, sizeof e) == sizeof e) {
            got = 1;
            if (e.type == EV_REL) {
                if (e.code == REL_X)
                    mx += e.value;
                if (e.code == REL_Y)
                    my += e.value;
                if (e.code == REL_WHEEL)
                    ;
            } else if (e.type == EV_ABS) {
                if (e.code == ABS_X)
                    mx = (e.value - abs_min_x) * (maxx - 1) / (abs_max_x - abs_min_x);
                if (e.code == ABS_Y)
                    my = (e.value - abs_min_y) * (maxy - 1) / (abs_max_y - abs_min_y);
            } else if (e.type == EV_KEY) {
                if ((e.code == BTN_LEFT || e.code == BTN_TOUCH || e.code == BTN_MOUSE) && e.value == 1)
                    *click = 1;
                if (e.code == BTN_TOUCH)
                    last_touch = e.value;
            }
        }
    }
    if (mx < 0)
        mx = 0;
    if (my < 0)
        my = 0;
    if (mx >= maxx)
        mx = maxx - 1;
    if (my >= maxy)
        my = maxy - 1;
    *x = mx;
    *y = my;
    return got || *click;
}
