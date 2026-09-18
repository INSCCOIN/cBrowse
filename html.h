#ifndef HTML_H
#define HTML_H
#include <stddef.h>

enum { MAX_LINKS = 64, MAX_HREF = 2048, MAX_TEXT = 96, MAX_IMG = 8, IMG_W = 48, IMG_H = 18 };

typedef struct {
    char href[MAX_HREF];
    char text[MAX_TEXT];
} Link;

typedef struct {
    char href[MAX_HREF];
    char alt[MAX_TEXT];
    unsigned char pix[IMG_W * IMG_H]; /* 0-7 color index */
    int ok;
} Img;

typedef struct {
    char *body;
    Link link[MAX_LINKS];
    Img img[MAX_IMG];
    int nlink, nimg;
    char title[80];
} Page;

void page_free(Page *p);
int html_parse(const char *html, Page *out);
void url_join(char *dst, size_t n, const char *base, const char *rel);

#endif
