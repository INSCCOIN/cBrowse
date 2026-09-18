#ifndef CBROWSE_MOUSE_H
#define CBROWSE_MOUSE_H

#include <sys/select.h>

int mouse_open(int max_x, int max_y);
void mouse_close(void);
int mouse_poll(int *x, int *y, int *click);
int mouse_max_fd(void);
void mouse_add_fds(fd_set *rf);

#endif
