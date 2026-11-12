#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "net.h"

/* send() 可能只送出一部分，必須迴圈送到完為止。
 * MSG_NOSIGNAL：對方已關閉時不要產生 SIGPIPE，改為回傳錯誤。 */
int send_all(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int vsend_line(int fd, const char *fmt, va_list ap)
{
    char buf[MAX_LINE];
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    if (n < 0) return -1;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    return send_all(fd, buf, (size_t)n);
}

int send_line(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vsend_line(fd, fmt, ap);
    va_end(ap);
    return rc;
}

void lb_init(linebuf_t *lb) { lb->len = 0; }

int lb_feed(linebuf_t *lb, const char *data, size_t n)
{
    if (lb->len + n > sizeof lb->buf) return -1;
    memcpy(lb->buf + lb->len, data, n);
    lb->len += n;
    return 0;
}

int lb_getline(linebuf_t *lb, char *out, size_t outsz)
{
    char *nl = memchr(lb->buf, '\n', lb->len);
    if (!nl) return 0;
    size_t L = (size_t)(nl - lb->buf);
    size_t c = L < outsz - 1 ? L : outsz - 1;
    memcpy(out, lb->buf, c);
    out[c] = '\0';
    if (c > 0 && out[c - 1] == '\r') out[c - 1] = '\0';
    memmove(lb->buf, nl + 1, lb->len - L - 1);
    lb->len -= L + 1;
    return 1;
}

/* 暱稱：1~NICK_MAX 位元組，不可含空白、冒號、控制字元（允許中文） */
int valid_nick(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > NICK_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch <= 0x20 || ch == 0x7f || ch == ':') return 0;
    }
    return 1;
}

void sanitize_text(char *s, size_t maxbytes)
{
    size_t n = strlen(s);
    if (n > maxbytes) {
        n = maxbytes;
        /* 往回退到 UTF-8 字元邊界（跳過 10xxxxxx 延續位元組） */
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
        s[n] = '\0';
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 0x20 || ch == 0x7f) s[i] = ' ';
    }
}
