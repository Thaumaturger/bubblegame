/*
 * main.c — 終端機版 Client
 *
 * 單執行緒，以 select() 同時監聽 stdin（鍵盤）與 socket（Server），
 * 逾時 50ms 用來重繪畫面與送心跳。協定與狀態處理在 core.c。
 *
 * 用法：./bubble_client [host] [port]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include "client.h"

static volatile sig_atomic_t g_sig = 0;

static void on_keys(const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == 0x04) { core_quit("按下 Ctrl+D，已離開遊戲"); return; }   /* Ctrl+D */
        if (c == 0x1b) {
            if (i + 2 < n && b[i + 1] == '[') {      /* 方向鍵：ESC [ A/B/C/D */
                if (!core_line_mode()) core_game_key(b[i + 2] + 256);
                i += 2;
            } else {
                core_cancel_chat();                  /* 單獨 ESC：取消聊天 */
            }
            continue;
        }
        if (CL.st == CS_HELLO_SENT || CL.st == CS_JOINING) continue;
        if (core_line_mode()) core_line_key(c);
        else core_game_key(c);
    }
}

static void on_signal(int sig) { (void)sig; g_sig = 1; }

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "7777";

    if (!isatty(STDIN_FILENO)) { fprintf(stderr, "請在終端機中執行 client\n"); return 1; }

    int sock = core_connect(host, port);
    if (sock < 0) { fprintf(stderr, "無法連線到 %s:%s\n", host, port); return 1; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;            /* 不設 SA_RESTART，讓 select 被中斷 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);             /* 避免 Ctrl+Z 讓終端機停在 raw 模式 */

    core_init(sock, host, port);
    ui_init();
    atexit(ui_restore);
    ui_render(&CL);

    while (!core_quitting()) {
        if (g_sig) { core_quit("收到中斷訊號（Ctrl+C），已離開遊戲"); break; }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {0, TICK_MS * 1000};
        int n = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            core_quit("select 發生錯誤");
            break;
        }

        if (n > 0 && FD_ISSET(sock, &rfds)) core_on_readable();

        if (n > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            /* 用 read() 而非 fgets()：stdio 緩衝會讓 select 看不到殘留輸入 */
            unsigned char kb[64];
            ssize_t r = read(STDIN_FILENO, kb, sizeof kb);
            if (r == 0) core_quit("輸入結束（EOF），已離開遊戲");
            else if (r > 0) on_keys(kb, (int)r);
        }

        core_periodic();

        if (CL.dirty || CL.st == CS_GAME || CL.st == CS_ENDED) {
            ui_render(&CL);
            CL.dirty = 0;
        }
    }

    core_finish();
    ui_restore();
    printf("%s\n", core_exit_reason());
    return 0;
}
