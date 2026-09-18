#define _GNU_SOURCE
#include "html.h"
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void page_free(Page *p)
{
    if (!p)
        return;
    free(p->body);
    memset(p, 0, sizeof *p);
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static const char *skip_ws(const char *s)
{
    while (*s && is_ws((unsigned char)*s))
        s++;
    return s;
}

static int tag_is(const char *s, const char *name)
{
    size_t n = strlen(name);
    if (strncasecmp(s, name, n))
        return 0;
    return !s[n] || !isalnum((unsigned char)s[n]);
}

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
    if (*len + n + 1 > *cap) {
        *cap = (*cap + n + 256) * 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = 0;
}

static void appch(char **buf, size_t *len, size_t *cap, char c)
{
    append(buf, len, cap, &c, 1);
}

static int entity(const char **sp)
{
    const char *s = *sp;
    if (!strncmp(s, "&amp;", 5)) {
        *sp = s + 5;
        return '&';
    }
    if (!strncmp(s, "&lt;", 4)) {
        *sp = s + 4;
        return '<';
    }
    if (!strncmp(s, "&gt;", 4)) {
        *sp = s + 4;
        return '>';
    }
    if (!strncmp(s, "&nbsp;", 6) || !strncmp(s, "&#160;", 6)) {
        *sp = s + 6;
        return ' ';
    }
    if (!strncmp(s, "&quot;", 6)) {
        *sp = s + 6;
        return '"';
    }
    if (s[0] == '&' && s[1] == '#' && isdigit((unsigned char)s[2])) {
        int v = 0;
        s += 2;
        while (isdigit((unsigned char)*s))
            v = v * 10 + *s++ - '0';
        if (*s == ';')
            s++;
        *sp = s;
        return v > 0 && v < 127 ? v : '?';
    }
    return 0;
}

static void grab_attr(const char *tag, const char *name, char *out, size_t n)
{
    char pat[32];
    const char *p;
    size_t i = 0;
    snprintf(pat, sizeof pat, "%s=", name);
    p = tag;
    out[0] = 0;
    while (*p) {
        if (!strncasecmp(p, pat, strlen(pat))) {
            p += strlen(pat);
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                while (*p && *p != q && i + 1 < n)
                    out[i++] = *p++;
            } else {
                while (*p && !is_ws((unsigned char)*p) && *p != '>' && i + 1 < n)
                    out[i++] = *p++;
            }
            out[i] = 0;
            return;
        }
        p++;
    }
}

void url_join(char *dst, size_t n, const char *base, const char *rel)
{
    char tmp[MAX_HREF], *cut;
    if (!rel || !rel[0]) {
        snprintf(dst, n, "%s", base ? base : "");
        return;
    }
    if (!strncmp(rel, "http://", 7) || !strncmp(rel, "https://", 8) || !strncmp(rel, "data:", 5)) {
        snprintf(dst, n, "%s", rel);
        return;
    }
    if (rel[0] == '/' && rel[1] == '/') {
        snprintf(dst, n, "https:%s", rel);
        return;
    }
    if (!base) {
        snprintf(dst, n, "%s", rel);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s", base);
    cut = strstr(tmp, "://");
    cut = cut ? cut + 3 : tmp; /* host */
    if (rel[0] == '/') {
        char *slash = strchr(cut, '/');
        if (slash)
            *slash = 0;
        snprintf(dst, n, "%s%s", tmp, rel);
        return;
    }
    {
        char *path = strchr(cut, '/');
        if (!path) {
            snprintf(dst, n, "%s/%s", tmp, rel);
            return;
        }
        cut = strrchr(path, '/');
        if (cut)
            cut[1] = 0;
        snprintf(dst, n, "%s%s", tmp, rel);
    }
}

int html_parse(const char *html, Page *out)
{
    const char *s = html ? html : "";
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int skip = 0, hide = 0, last_sp = 1, in_a = 0, ai = -1;
    char href[MAX_HREF];
    memset(out, 0, sizeof *out);
    href[0] = 0;
    while (*s) {
        if (*s == '&') {
            int e = entity(&s);
            if (e) {
                if (e == ' ' && last_sp)
                    continue;
                appch(&buf, &len, &cap, (char)e);
                last_sp = (e == ' ');
                if (in_a && ai >= 0) {
                    size_t L = strlen(out->link[ai].text);
                    if (L + 1 < MAX_TEXT) {
                        out->link[ai].text[L] = (char)e;
                        out->link[ai].text[L + 1] = 0;
                    }
                }
                continue;
            }
        }
        if (*s != '<') {
            if (!skip) {
                char c = *s;
                if (is_ws((unsigned char)c))
                    c = ' ';
                if (!(c == ' ' && last_sp)) {
                    appch(&buf, &len, &cap, c);
                    last_sp = c == ' ';
                    if (in_a && ai >= 0 && c != ' ') {
                        size_t L = strlen(out->link[ai].text);
                        if (L + 1 < MAX_TEXT) {
                            out->link[ai].text[L] = c;
                            out->link[ai].text[L + 1] = 0;
                        }
                    }
                }
            }
            s++;
            continue;
        }
        {
            const char *end = strchr(s, '>');
            char tag[192];
            size_t tl;
            int slash;
            if (!end) {
                s++;
                continue;
            }
            tl = (size_t)(end - s - 1);
            if (tl >= sizeof tag)
                tl = sizeof tag - 1;
            memcpy(tag, s + 1, tl);
            tag[tl] = 0;
            s = end + 1;
            slash = tag[0] == '/';
            {
                char *name = tag + slash;
                while (*name == ' ')
                    name++;
                if (tag_is(name, "script") || tag_is(name, "style")) {
                    skip = !slash;
                    continue;
                }
                if (skip)
                    continue;
                {
                    char st[160];
                    grab_attr(name, "style", st, sizeof st);
                    if (!slash && (strcasestr(name, "hidden") || strcasestr(st, "display:none") ||
                                   strcasestr(st, "display: none") || strcasestr(st, "visibility:hidden")))
                        hide++;
                    if (slash && hide)
                        hide--;
                    if (hide)
                        continue;
                }
                if (!slash && tag_is(name, "title") && !out->title[0]) {
                    const char *te = strcasestr(s, "</title>");
                    if (te) {
                        size_t n = (size_t)(te - s);
                        if (n > sizeof out->title - 1)
                            n = sizeof out->title - 1;
                        memcpy(out->title, s, n);
                        out->title[n] = 0;
                    }
                }
                if (!slash && (tag_is(name, "p") || tag_is(name, "div") || tag_is(name, "br") ||
                               tag_is(name, "tr") || tag_is(name, "li") || tag_is(name, "h1") ||
                               tag_is(name, "h2") || tag_is(name, "h3") || tag_is(name, "br/"))) {
                    if (len && buf[len - 1] != '\n')
                        appch(&buf, &len, &cap, '\n');
                    last_sp = 1;
                }
                if (!slash && tag_is(name, "meta")) {
                    char prop[48], cont[MAX_HREF];
                    grab_attr(name, "property", prop, sizeof prop);
                    if (!prop[0])
                        grab_attr(name, "name", prop, sizeof prop);
                    grab_attr(name, "content", cont, sizeof cont);
                    if (!strcasecmp(prop, "og:image") && cont[0] && out->nimg < MAX_IMG) {
                        Img *im = &out->img[out->nimg];
                        memset(im, 0, sizeof *im);
                        snprintf(im->href, sizeof im->href, "%s", cont);
                        snprintf(im->alt, sizeof im->alt, "og");
                        out->nimg++;
                    }
                    if (!strcasecmp(prop, "og:title") && cont[0] && !out->title[0])
                        snprintf(out->title, sizeof out->title, "%s", cont);
                }
                if (!slash && tag_is(name, "img") && out->nimg < MAX_IMG) {
                    Img *im = &out->img[out->nimg];
                    memset(im, 0, sizeof *im);
                    grab_attr(name, "src", im->href, sizeof im->href);
                    grab_attr(name, "alt", im->alt, sizeof im->alt);
                    if (im->href[0]) {
                        char mark[16];
                        snprintf(mark, sizeof mark, "{img%d}", out->nimg + 1);
                        append(&buf, &len, &cap, mark, strlen(mark));
                        last_sp = 0;
                        out->nimg++;
                    }
                }
                if (!slash && tag_is(name, "a")) {
                    grab_attr(name, "href", href, sizeof href);
                    if (href[0] && out->nlink < MAX_LINKS) {
                        ai = out->nlink++;
                        snprintf(out->link[ai].href, sizeof out->link[ai].href, "%s", href);
                        out->link[ai].text[0] = 0;
                        in_a = 1;
                        {
                            char mark[16];
                            snprintf(mark, sizeof mark, "[%d]", ai + 1);
                            append(&buf, &len, &cap, mark, strlen(mark));
                            last_sp = 0;
                        }
                    }
                }
                if (slash && tag_is(name, "a"))
                    in_a = 0;
            }
        }
    }
    out->body = buf ? buf : calloc(1, 1);
    return 0;
}
