/*
 * net.h — 共用網路工具：完整送出、以行切割的接收緩衝區、字串檢查
 */
#ifndef NET_H
#define NET_H

#include <stdarg.h>
#include <stddef.h>
#include "protocol.h"

/* TCP 是位元串流：一次 recv() 可能收到半行或多行，需累積後再切行 */
typedef struct {
    char   buf[MAX_LINE * 2];
    size_t len;
} linebuf_t;

int  send_all(int fd, const void *data, size_t n);
int  vsend_line(int fd, const char *fmt, va_list ap);
int  send_line(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

void lb_init(linebuf_t *lb);
int  lb_feed(linebuf_t *lb, const char *data, size_t n);   /* -1 = 單行過長 */
int  lb_getline(linebuf_t *lb, char *out, size_t outsz);   /* 1 = 取得一行 */

int  valid_nick(const char *s);                 /* Client 與 Server 用同一套規則 */
void sanitize_text(char *s, size_t maxbytes);   /* 移除控制字元並截斷（不切斷 UTF-8 字元） */

#endif
