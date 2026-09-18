/* cBrowse — framebuffer browser. Not a TTY app. */
#define _GNU_SOURCE
#include "html.h"
#include "mouse.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define HIST 16
#define IW 200
#define IH 90
#define LOGPATH "/home/cbrowse.log"
#define CURW 11
#define CURH 13

static int fb = -1;
static unsigned char *map;
static size_t maplen;
static unsigned W, H, BPP, LINE;
static struct termios oldt;
static int raw_on, run = 1, off, img_ok, img_w, img_h, mx, my;
static int cur_on, cur_x, cur_y;
static uint16_t cur_save[CURH][CURW];
static FILE *logf;
static char url[512] = "https://example.com";
static char hist[HIST][512];
static int hist_i = -1;
static char msg[96] = "g url   1-9 link   b back   q";
static Page page;
static uint16_t ibuf[IH][IW];
static uint16_t C_DESK, C_FACE, C_NAVY, C_WHITE, C_BLACK, C_BLUE, C_GRAY, C_SHAD, C_LIT, C_RED;

typedef struct {
    int x, y, w, h, id;
} Hit;
static Hit hits[80];
static int nhit;

static uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static void log_line(const char *s)
{
    if (!logf)
        return;
    fprintf(logf, "%s\n", s);
    fflush(logf);
}

static void log_init(void)
{
    unlink(LOGPATH);
    logf = fopen(LOGPATH, "w");
    log_line("cBrowse start");
}

static void log_close(void)
{
    if (!logf)
        return;
    log_line("cBrowse exit — log saved");
    fclose(logf);
    logf = NULL;
}

static uint16_t get_px(int x, int y)
{
    unsigned char *p;
    if ((unsigned)x >= W || (unsigned)y >= H)
        return 0;
    p = map + (size_t)y * LINE + (size_t)x * (BPP / 8);
    if (BPP == 16)
        return ((uint16_t *)p)[0];
    if (BPP == 32)
        return rgb565(p[2], p[1], p[0]);
    return 0;
}

static void px(int x, int y, uint16_t c)
{
    unsigned char *p;
    if ((unsigned)x >= W || (unsigned)y >= H)
        return;
    p = map + (size_t)y * LINE + (size_t)x * (BPP / 8);
    if (BPP == 16)
        ((uint16_t *)p)[0] = c;
    else if (BPP == 32) {
        p[0] = (unsigned char)((c & 0x1f) << 3);
        p[1] = (unsigned char)(((c >> 5) & 0x3f) << 2);
        p[2] = (unsigned char)(((c >> 11) & 0x1f) << 3);
        p[3] = 0;
    }
}

static void fill(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            px(x + i, y + j, c);
}

static void cursor_hide(void)
{
    int x, y;
    if (!cur_on)
        return;
    for (y = 0; y < CURH; y++)
        for (x = 0; x < CURW; x++)
            px(cur_x + x, cur_y + y, cur_save[y][x]);
    cur_on = 0;
}

static void cursor_show(void)
{
    static const char spr[CURH][CURW + 1] = {
        "X          ",
        "XX         ",
        "X.X        ",
        "X..X       ",
        "X...X      ",
        "X....X     ",
        "X.....X    ",
        "X......X   ",
        "X.....X    ",
        "X..XX      ",
        "X.X X      ",
        "   X X     ",
        "    X      ",
    };
    int x, y;
    cursor_hide();
    cur_x = mx;
    cur_y = my;
    for (y = 0; y < CURH; y++)
        for (x = 0; x < CURW; x++)
            cur_save[y][x] = get_px(cur_x + x, cur_y + y);
    for (y = 0; y < CURH; y++)
        for (x = 0; x < CURW; x++) {
            char c = spr[y][x];
            if (c == 'X')
                px(cur_x + x, cur_y + y, C_BLACK);
            else if (c == '.')
                px(cur_x + x, cur_y + y, C_WHITE);
        }
    cur_on = 1;
}

static void hline(int x, int y, int w, uint16_t c)
{
    int i;
    for (i = 0; i < w; i++)
        px(x + i, y, c);
}

static void vline(int x, int y, int h, uint16_t c)
{
    int i;
    for (i = 0; i < h; i++)
        px(x, y + i, c);
}

static void raised(int x, int y, int w, int h, uint16_t face)
{
    fill(x, y, w, h, face);
    hline(x, y, w, C_LIT);
    vline(x, y, h, C_LIT);
    hline(x, y + h - 1, w, C_SHAD);
    vline(x + w - 1, y, h, C_SHAD);
}

static const unsigned char FONT[96][5] = {
    {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},
    {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},
    {0,5,3,0,0},{0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,8,0x3e,8,0x14},
    {8,8,0x3e,8,8},{0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2},{0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},
    {0,0x56,0x36,0,0},{8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8},{2,1,0x51,9,6},{0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},
    {0x3e,0x41,0x49,0x49,0x7a},{0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},
    {0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
    {0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7f,0x41,0x41,0},
    {2,4,8,0x10,0x20},{0,0x41,0x41,0x7f,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},
    {8,0x7e,9,1,2},{0x0c,0x52,0x52,0x52,0x3e},{0x7f,8,4,4,0x78},
    {0,0x44,0x7d,0x40,0},{0x20,0x40,0x44,0x3d,0},{0x7f,0x10,0x28,0x44,0},
    {0,0x41,0x7f,0x40,0},{0x7c,4,0x18,4,0x78},{0x7c,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38},{0x7c,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7c},
    {0x7c,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},{4,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
    {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},
};

static void text(int x, int y, const char *s, uint16_t c)
{
    while (*s) {
        unsigned char ch = (unsigned char)*s++;
        int gx, gy;
        if (ch < 32 || ch > 126)
            ch = '?';
        for (gx = 0; gx < 5; gx++) {
            unsigned char col = FONT[ch - 32][gx];
            for (gy = 0; gy < 7; gy++)
                if (col & (1 << gy))
                    px(x + gx, y + gy, c);
        }
        x += 6;
    }
}

static void hit_add(int x, int y, int w, int h, int id)
{
    if (nhit >= 80)
        return;
    hits[nhit].x = x;
    hits[nhit].y = y;
    hits[nhit].w = w;
    hits[nhit].h = h;
    hits[nhit].id = id;
    nhit++;
}

static int hit_at(int x, int y)
{
    int i;
    for (i = nhit - 1; i >= 0; i--)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return hits[i].id;
    return 0;
}

static void btn(int x, int y, const char *lab, int id)
{
    int n = (int)strlen(lab) * 6 + 8;
    raised(x, y, n, 14, C_FACE);
    text(x + 4, y + 3, lab, C_BLACK);
    hit_add(x, y, n, 14, id);
}

static int fb_open(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    fb = open("/dev/fb0", O_RDWR);
    if (fb < 0)
        return -1;
    ioctl(fb, FBIOGET_VSCREENINFO, &v);
    ioctl(fb, FBIOGET_FSCREENINFO, &f);
    W = v.xres;
    H = v.yres;
    BPP = v.bits_per_pixel;
    LINE = f.line_length;
    maplen = f.smem_len ? f.smem_len : (size_t)LINE * H;
    map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    return map == MAP_FAILED ? -1 : 0;
}

static void raw(int on)
{
    struct termios t;
    if (on) {
        tcgetattr(0, &oldt);
        t = oldt;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        raw_on = 1;
    } else if (raw_on)
        tcsetattr(0, TCSANOW, &oldt);
}

static void push_hist(const char *u)
{
    if (hist_i >= 0 && !strcmp(hist[hist_i], u))
        return;
    if (hist_i + 1 < HIST)
        hist_i++;
    else {
        memmove(hist[0], hist[1], sizeof hist[0] * (HIST - 1));
        hist_i = HIST - 1;
    }
    snprintf(hist[hist_i], sizeof hist[0], "%s", u);
}

static int fetch(const char *u, const char *path, int tmax, int maxb)
{
    char cmd[800];
    snprintf(cmd, sizeof cmd,
             "curl -L --max-time %d -sS -A 'cBrowse/0.1' --max-filesize %d -o '%s' '%s' 2>/dev/null",
             tmax, maxb, path, u);
    return system(cmd) == 0 ? 0 : -1;
}

static void load_image(void)
{
    char path[] = "/tmp/cbrowse.img";
    char rawp[] = "/tmp/cbrowse.rgb";
    char cmd[256];
    FILE *f;
    unsigned char *rgb;
    int x, y;
    img_ok = 0;
    if (!page.nimg)
        return;
    if (fetch(page.img[0].href, path, 8, 200000) != 0)
        return;
    snprintf(cmd, sizeof cmd,
             "ffmpeg -nostdin -y -loglevel error -i %s -vf scale=%d:%d -f rawvideo -pix_fmt rgb24 %s",
             path, IW, IH, rawp);
    if (system(cmd) != 0)
        return;
    f = fopen(rawp, "rb");
    if (!f)
        return;
    rgb = malloc(IW * IH * 3);
    if (!rgb || fread(rgb, 1, IW * IH * 3, f) != IW * IH * 3) {
        free(rgb);
        fclose(f);
        return;
    }
    fclose(f);
    for (y = 0; y < IH; y++)
        for (x = 0; x < IW; x++) {
            unsigned char *p = rgb + (y * IW + x) * 3;
            ibuf[y][x] = rgb565(p[0], p[1], p[2]);
        }
    free(rgb);
    img_w = IW;
    img_h = IH;
    img_ok = 1;
}

static void load(const char *u)
{
    char path[] = "/tmp/cbrowse.html";
    char *raw = NULL;
    FILE *f;
    long sz;
    int i;
    snprintf(msg, sizeof msg, "Loading...");
    {
        char l[600];
        snprintf(l, sizeof l, "GET %s", u);
        log_line(l);
    }
    if (fetch(u, path, 25, 2000000) != 0) {
        snprintf(msg, sizeof msg, "fetch failed");
        log_line("fetch failed");
        return;
    }
    f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    raw = malloc((size_t)sz + 1);
    if (!raw) {
        fclose(f);
        return;
    }
    sz = (long)fread(raw, 1, (size_t)sz, f);
    raw[sz] = 0;
    fclose(f);
    page_free(&page);
    html_parse(raw, &page);
    free(raw);
    for (i = 0; i < page.nlink; i++) {
        char j[MAX_HREF];
        url_join(j, sizeof j, u, page.link[i].href);
        snprintf(page.link[i].href, sizeof page.link[i].href, "%s", j);
    }
    for (i = 0; i < page.nimg; i++) {
        char j[MAX_HREF];
        url_join(j, sizeof j, u, page.img[i].href);
        snprintf(page.img[i].href, sizeof page.img[i].href, "%s", j);
    }
    snprintf(url, sizeof url, "%s", u);
    push_hist(u);
    off = 0;
    load_image();
    snprintf(msg, sizeof msg, "%d links  %s", page.nlink, page.title[0] ? page.title : "");
    {
        char l[160];
        snprintf(l, sizeof l, "ok links=%d img=%d title=%s", page.nlink, page.nimg,
                 page.title[0] ? page.title : "-");
        log_line(l);
    }
}

static void follow(int n)
{
    if (n >= 1 && n <= page.nlink)
        load(page.link[n - 1].href);
}

static void draw(void)
{
    int x0 = 4, y0 = 44, bw = (int)W - 8, bh = (int)H - 60;
    int row, col, skip;
    const char *s;
    cursor_hide();
    nhit = 0;
    fill(0, 0, (int)W, (int)H, C_DESK);
    fill(2, 2, (int)W - 4, (int)H - 4, C_NAVY);
    text(8, 6, page.title[0] ? page.title : "cBrowse", C_WHITE);
    text((int)W - 18, 6, "x", C_WHITE);
    hit_add((int)W - 22, 4, 16, 12, -6);
    fill(4, 18, (int)W - 8, (int)H - 22, C_FACE);
    btn(8, 20, "Back", -1);
    btn(58, 20, "Go", -2);
    btn(92, 20, "Reload", -7);
    btn(150, 20, "Quit", -6);
    raised(8, 36, (int)W - 16, 12, C_WHITE);
    text(10, 38, url, C_BLACK);
    hit_add(8, 36, (int)W - 16, 12, -2);
    fill(x0, y0, bw, bh, C_WHITE);
    row = y0 + 2;
    if (img_ok) {
        int x, y, maxw = bw - 4;
        int dw = img_w < maxw ? img_w : maxw;
        for (y = 0; y < img_h && row + y < y0 + bh - 2; y++)
            for (x = 0; x < dw; x++)
                px(x0 + 2 + x, row + y, ibuf[y][x]);
        row += img_h + 4;
    }
    s = page.body ? page.body : "";
    col = x0 + 2;
    skip = off;
    while (*s && row < y0 + bh - 10) {
        if (*s == '\n') {
            if (skip)
                skip--;
            else {
                row += 9;
                col = x0 + 2;
            }
            s++;
            continue;
        }
        if (skip) {
            s++;
            continue;
        }
        if (col > x0 + bw - 10) {
            row += 9;
            col = x0 + 2;
            if (row >= y0 + bh - 10)
                break;
        }
        if (*s == '[') {
            const char *p = s + 1;
            int n = 0;
            while (*p >= '0' && *p <= '9')
                n = n * 10 + *p++ - '0';
            if (*p == ']' && n >= 1 && n <= page.nlink) {
                int x1 = col;
                char tmp[8];
                snprintf(tmp, sizeof tmp, "[%d]", n);
                text(col, row, tmp, C_BLUE);
                x1 = col + (int)strlen(tmp) * 6;
                hline(col, row + 8, x1 - col, C_BLUE);
                hit_add(col, row, x1 - col, 9, n);
                col = x1;
                s = p + 1;
                continue;
            }
        }
        {
            char ch[2] = {*s++, 0};
            text(col, row, ch, C_BLACK);
            col += 6;
        }
    }
    fill(4, (int)H - 16, (int)W - 8, 14, C_FACE);
    hline(4, (int)H - 16, (int)W - 8, C_LIT);
    text(8, (int)H - 12, msg, C_BLACK);
    cursor_show();
}

static void type_url(void);

static void do_id(int id)
{
    if (id > 0)
        follow(id);
    else if (id == -1 && hist_i > 0) {
        hist_i--;
        load(hist[hist_i]);
        hist_i--;
    } else if (id == -2)
        type_url();
    else if (id == -6)
        run = 0;
    else if (id == -7)
        load(url);
}

static void type_url(void)
{
    char buf[512];
    int n = 0;
    buf[0] = 0;
    snprintf(msg, sizeof msg, "URL: ");
    draw();
    for (;;) {
        unsigned char ch = 0;
        if (read(0, &ch, 1) != 1)
            continue;
        if (ch == '\n' || ch == '\r')
            break;
        if (ch == 0x1b) {
            buf[0] = 0;
            break;
        }
        if ((ch == 8 || ch == 127) && n) {
            buf[--n] = 0;
        } else if (ch >= 32 && ch < 127 && n < 500) {
            buf[n++] = (char)ch;
            buf[n] = 0;
        }
        snprintf(msg, sizeof msg, "URL: %s", buf);
        draw();
    }
    if (!buf[0])
        return;
    if (!strstr(buf, "://")) {
        char t[520];
        snprintf(t, sizeof t, "https://%s", buf);
        load(t);
    } else
        load(buf);
}

int main(int argc, char **argv)
{
    log_init();
    if (fb_open() < 0) {
        log_line("fb0 open failed");
        log_close();
        fprintf(stderr, "cBrowse needs /dev/fb0 (not a terminal UI)\n");
        return 1;
    }
    C_DESK = rgb565(0, 128, 128);
    C_FACE = rgb565(192, 192, 192);
    C_NAVY = rgb565(0, 0, 128);
    C_WHITE = rgb565(255, 255, 255);
    C_BLACK = rgb565(0, 0, 0);
    C_BLUE = rgb565(0, 0, 200);
    C_GRAY = rgb565(160, 160, 160);
    C_SHAD = rgb565(80, 80, 80);
    C_LIT = rgb565(255, 255, 255);
    C_RED = rgb565(200, 0, 0);
    if (argc > 1)
        snprintf(url, sizeof url, "%s", argv[1]);
    raw(1);
    mouse_open((int)W, (int)H);
    mx = (int)W / 2;
    my = (int)H / 2;
    load(url);
    draw();
    while (run) {
        unsigned char ch = 0;
        fd_set rf;
        struct timeval tv = {0, 40000};
        int mfd, click = 0, dirty = 0;
        FD_ZERO(&rf);
        FD_SET(0, &rf);
        mouse_add_fds(&rf);
        mfd = mouse_max_fd();
        if (mfd < 0)
            mfd = 0;
        if (select(mfd + 1, &rf, NULL, NULL, &tv) > 0) {
            if (FD_ISSET(0, &rf))
                read(0, &ch, 1);
        }
        if (mouse_poll(&mx, &my, &click)) {
            cursor_show();
        }
        if (click) {
            int id = hit_at(mx, my);
            char l[80];
            snprintf(l, sizeof l, "click %d,%d id=%d", mx, my, id);
            log_line(l);
            if (id)
                do_id(id);
            dirty = 1;
        }
        if (!ch && !dirty)
            continue;
        if (ch == 'q')
            run = 0;
        else if (ch == 'g')
            type_url();
        else if (ch == 'b')
            do_id(-1);
        else if (ch == 'r')
            load(url);
        else if (ch == 'j' || ch == 0x0e)
            off++;
        else if ((ch == 'k' || ch == 0x10) && off)
            off--;
        else if (ch >= '1' && ch <= '9')
            follow(ch - '0');
        else if (ch == 0x1b) {
            unsigned char s[6] = {0};
            struct timeval t2 = {0, 60000};
            FD_ZERO(&rf);
            FD_SET(0, &rf);
            if (select(1, &rf, NULL, NULL, &t2) > 0)
                read(0, s, 6);
            if (s[0] == '[' && s[1] == 'B')
                off++;
            if (s[0] == '[' && s[1] == 'A' && off)
                off--;
        }
        if (run)
            draw();
    }
    raw(0);
    mouse_close();
    log_close();
    page_free(&page);
    munmap(map, maplen);
    close(fb);
    return 0;
}
