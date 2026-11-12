/*
 * main.c — Server 主程式：接受連線、client thread、大廳指令
 *
 * 用法：./server [port]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "server.h"
#include "net.h"

static volatile sig_atomic_t g_stop = 0;
static client_t       *g_clients = NULL;
static int             g_nclients = 0;
static int             g_next_id = 1;
static pthread_mutex_t g_cl_mtx = PTHREAD_MUTEX_INITIALIZER;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

void srv_log(const char *fmt, ...)
{
    char buf[512];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("[%02d:%02d:%02d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, buf);
    fflush(stdout);
}

/* 送出失敗（對方斷線或送出逾時）就關閉 socket，讓該 client 的 recv 迴圈結束並走斷線流程 */
void cli_send(client_t *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&c->send_mtx);
    int rc = vsend_line(c->fd, fmt, ap);
    pthread_mutex_unlock(&c->send_mtx);
    va_end(ap);
    if (rc < 0) shutdown(c->fd, SHUT_RDWR);
}

static void send_err(client_t *c, int code, const char *msg)
{
    cli_send(c, "%d %d %s", S_ERR, code, msg);
}

static void post(room_t *r, client_t *c, ev_type_t t, int arg, const char *text)
{
    event_t *e = calloc(1, sizeof *e);
    if (!e) return;
    e->type = t;
    e->cli = c;
    e->arg = arg;
    if (text) snprintf(e->text, sizeof e->text, "%s", text);
    room_post(r, e);
}

static int parse_int(const char *s, int *out)
{
    if (!s || !*s) return -1;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end) return -1;
    *out = (int)v;
    return 0;
}

static int is_known_code(int code)
{
    switch (code) {
    case C_HELLO: case C_LIST: case C_CREATE: case C_JOIN: case C_LEAVE: case C_READY:
    case C_MOVE: case C_BOMB: case C_CHAT: case C_SURRENDER: case C_RESYNC:
        return 1;
    }
    return 0;
}

static void set_session(client_t *c, sess_state_t st, room_t *r)
{
    pthread_mutex_lock(&c->mtx);
    c->state = st;
    c->room = r;
    pthread_mutex_unlock(&c->mtx);
}

/* 回傳 -1 表示要結束此連線 */
static int handle_line(client_t *c, char *line)
{
    char *save = NULL;
    char *tok = strtok_r(line, " ", &save);
    int code;
    if (!tok) return 0;
    if (parse_int(tok, &code) < 0) {
        send_err(c, E_FORMAT, "bad_format");
        return ++c->bad > 20 ? -1 : 0;
    }
    /* 任何狀態都可用的訊息 */
    if (code == C_PING) { cli_send(c, "%d", S_PONG); return 0; }
    if (code == C_QUIT) return -1;

    char *rest = save ? save : "";          /* 聊天訊息要整行剩餘文字 */
    char chat[CHAT_MAX + 8];
    snprintf(chat, sizeof chat, "%s", rest);
    char *a1 = strtok_r(NULL, " ", &save);

    pthread_mutex_lock(&c->mtx);
    sess_state_t st = c->state;
    room_t *room = c->room;
    pthread_mutex_unlock(&c->mtx);

    int v;
    room_t *r;
    switch (st) {
    case SS_CONNECTED:
        if (code != C_HELLO) { send_err(c, E_STATE, "hello_first"); break; }
        if (!a1 || !valid_nick(a1)) { send_err(c, E_NICK, "bad_nick"); break; }
        snprintf(c->nick, sizeof c->nick, "%s", a1);
        set_session(c, SS_LOBBY, NULL);
        cli_send(c, "%d %d", S_WELCOME, c->id);
        srv_log("client #%d is now '%s'", c->id, c->nick);
        break;

    case SS_LOBBY:
        switch (code) {
        case C_LIST: {
            char buf[MAX_LINE];
            room_list_line(buf, sizeof buf);
            cli_send(c, "%s", buf);
            break;
        }
        case C_CREATE:
            if (parse_int(a1, &v) < 0 || (v != MODE_FFA && v != MODE_TEAM)) { send_err(c, E_FORMAT, "bad_mode"); break; }
            if (!(r = room_claim(v))) { send_err(c, E_FULL, "no_free_room"); break; }
            set_session(c, SS_JOINING, r);
            post(r, c, EV_JOIN, 0, NULL);
            break;
        case C_JOIN:
            if (parse_int(a1, &v) < 0) { send_err(c, E_FORMAT, "bad_room_id"); break; }
            if (!(r = room_find(v))) { send_err(c, E_NO_ROOM, "no_room"); break; }
            set_session(c, SS_JOINING, r);
            post(r, c, EV_JOIN, 0, NULL);
            break;
        default:
            send_err(c, is_known_code(code) ? E_STATE : E_UNKNOWN, "not_in_room");
        }
        break;

    case SS_JOINING:
        send_err(c, E_STATE, "joining");
        break;

    case SS_IN_ROOM:
        switch (code) {
        case C_LEAVE:     post(room, c, EV_LEAVE, 0, NULL); break;
        case C_BOMB:      post(room, c, EV_BOMB, 0, NULL); break;
        case C_SURRENDER: post(room, c, EV_SURRENDER, 0, NULL); break;
        case C_RESYNC:    post(room, c, EV_RESYNC, 0, NULL); break;
        case C_READY:
            if (parse_int(a1, &v) < 0 || (v != 0 && v != 1)) { send_err(c, E_FORMAT, "bad_ready"); break; }
            post(room, c, EV_READY, v, NULL);
            break;
        case C_MOVE:
            if (!a1 || strlen(a1) != 1 || !(v = dir_from_char(a1[0]))) { send_err(c, E_FORMAT, "bad_dir"); break; }
            post(room, c, EV_MOVE, v, NULL);
            break;
        case C_CHAT:
            sanitize_text(chat, CHAT_MAX);
            if (chat[0]) post(room, c, EV_CHAT, 0, chat);
            break;
        default:
            send_err(c, is_known_code(code) ? E_STATE : E_UNKNOWN, "in_room");
        }
        break;
    }
    return 0;
}

/* 斷線時：若還在房間（或正在加入），通知房間並等待房間放手，才能安全釋放 */
static void detach_from_room(client_t *c)
{
    pthread_mutex_lock(&c->mtx);
    room_t *r = c->room;
    c->released = 0;
    pthread_mutex_unlock(&c->mtx);
    if (!r) return;

    post(r, c, EV_DISCONNECT, 0, NULL);
    pthread_mutex_lock(&c->mtx);
    while (!c->released) pthread_cond_wait(&c->cv, &c->mtx);
    pthread_mutex_unlock(&c->mtx);
}

static void *client_thread(void *arg)
{
    client_t *c = arg;
    linebuf_t *lb = malloc(sizeof *lb);
    char data[1024], line[MAX_LINE];
    const char *why = "closed";

    lb_init(lb);
    for (;;) {
        ssize_t n = recv(c->fd, data, sizeof data, 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            why = (errno == EAGAIN || errno == EWOULDBLOCK) ? "timeout" : "error";
            break;
        }
        if (lb_feed(lb, data, (size_t)n) < 0) {
            send_err(c, E_FORMAT, "line_too_long");
            why = "line_too_long";
            break;
        }
        while (lb_getline(lb, line, sizeof line)) {
            if (handle_line(c, line) < 0) { why = "quit"; goto out; }
        }
    }
out:
    srv_log("client #%d (%s) disconnected: %s", c->id, c->nick[0] ? c->nick : "-", why);
    detach_from_room(c);

    pthread_mutex_lock(&g_cl_mtx);
    for (client_t **pp = &g_clients; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    g_nclients--;
    pthread_mutex_unlock(&g_cl_mtx);

    close(c->fd);
    pthread_mutex_destroy(&c->send_mtx);
    pthread_mutex_destroy(&c->mtx);
    pthread_cond_destroy(&c->cv);
    free(lb);
    free(c);
    return NULL;
}

static void setup_client_socket(int fd)
{
    int one = 1;
    struct timeval rcv = {TIMEOUT_SEC, 0}, snd = {3, 0};
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* 即時遊戲：關閉 Nagle */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);    /* 心跳逾時偵測 */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);    /* 避免卡在送給慢速 client */
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(ls, 16) < 0) { perror("listen"); return 1; }

    rooms_init();
    srv_log("BubbleBrawl server listening on port %d (Ctrl+C to stop)", port);

    while (!g_stop) {
        /* 用 poll 加逾時，確保 Ctrl+C 後能及時離開迴圈 */
        struct pollfd pfd = {ls, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;

        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int fd = accept(ls, (struct sockaddr *)&ca, &cl);
        if (fd < 0) continue;
        setup_client_socket(fd);

        pthread_mutex_lock(&g_cl_mtx);
        if (g_nclients >= MAX_CLIENTS) {
            pthread_mutex_unlock(&g_cl_mtx);
            send_line(fd, "%d %d %s", S_ERR, E_SERVER_FULL, "server_full");
            close(fd);
            continue;
        }
        client_t *c = calloc(1, sizeof *c);
        c->fd = fd;
        c->id = g_next_id++;
        c->state = SS_CONNECTED;
        pthread_mutex_init(&c->send_mtx, NULL);
        pthread_mutex_init(&c->mtx, NULL);
        pthread_cond_init(&c->cv, NULL);
        c->next = g_clients;
        g_clients = c;
        g_nclients++;
        pthread_mutex_unlock(&g_cl_mtx);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip);
        srv_log("client #%d connected from %s:%d", c->id, ip, ntohs(ca.sin_port));

        pthread_t th;
        pthread_create(&th, NULL, client_thread, c);
        pthread_detach(th);
    }

    /* 正常關閉：通知所有 client */
    srv_log("shutting down, notifying clients...");
    pthread_mutex_lock(&g_cl_mtx);
    for (client_t *c = g_clients; c; c = c->next) {
        cli_send(c, "%d", S_SHUTDOWN);
        shutdown(c->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_cl_mtx);
    close(ls);
    return 0;
}
