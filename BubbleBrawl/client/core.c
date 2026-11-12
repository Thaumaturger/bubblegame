/*
 * core.c — Client 共用核心：連線、Server 訊息處理、本地狀態（stateful client）、指令與按鍵邏輯
 *
 * 終端機版（main.c + ui.c）與圖形版（gui.c）共用這一份，
 * 兩者只差在「怎麼讀輸入」與「怎麼畫畫面」。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "client.h"
#include "net.h"

cl_t             CL;
static linebuf_t g_lb;
static int       g_quit = 0;
static int       g_conn_ok = 1;
static char      g_exit_reason[160] = "再見！";

/* ---------------------------------------------------------------- 工具 */

long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

void cl_log(cl_t *cl, const char *fmt, ...)
{
    if (cl->log_n == LOG_N) {
        memmove(cl->log[0], cl->log[1], sizeof cl->log[0] * (LOG_N - 1));
        cl->log_n--;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cl->log[cl->log_n++], sizeof cl->log[0], fmt, ap);
    va_end(ap);
    cl->dirty = 1;
}

const char *seat_name(const cl_t *cl, int seat)
{
    static char buf[MAX_PLAYERS][NICK_MAX + 8];
    if (seat < 0 || seat >= MAX_PLAYERS) return "?";
    if (cl->roster[seat].used) snprintf(buf[seat], sizeof buf[seat], "%s", cl->roster[seat].nick);
    else                       snprintf(buf[seat], sizeof buf[seat], "P%d", seat + 1);
    return buf[seat];
}

void core_quit(const char *reason)
{
    if (g_quit) return;
    g_quit = 1;
    snprintf(g_exit_reason, sizeof g_exit_reason, "%s", reason);
}

static void csend(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void csend(const char *fmt, ...)
{
    if (!g_conn_ok) return;
    va_list ap;
    va_start(ap, fmt);
    int rc = vsend_line(CL.sock, fmt, ap);
    va_end(ap);
    if (rc < 0) {
        g_conn_ok = 0;
        core_quit("送出資料失敗：與伺服器的連線中斷");
    }
}

static const char *err_text(int code)
{
    switch (code) {
    case E_FORMAT:      return "訊息格式錯誤";
    case E_UNKNOWN:     return "伺服器不認得這個指令";
    case E_STATE:       return "目前狀態無法執行這個動作";
    case E_NO_ROOM:     return "房間不存在";
    case E_FULL:        return "房間已滿，或沒有可用的空房間";
    case E_BUSY:        return "該房間正在遊戲中";
    case E_NICK:        return "暱稱不合法（1~24 位元組，不可含空白或冒號）";
    case E_SERVER_FULL: return "伺服器人數已滿";
    case E_ACTION:      return "這個動作不合法";
    }
    return "未知錯誤";
}

static void request_resync(const char *why)
{
    if (CL.resync_pending) return;
    CL.resync_pending = 1;
    csend("%d", C_RESYNC);
    cl_log(&CL, "\033[33m[同步]\033[0m %s，向伺服器要求完整狀態", why);
}

/* ---------------------------------------------------------- 遊戲事件 */

static int valid_seat(int s) { return s >= 0 && s < MAX_PLAYERS; }

/* 套用一個遊戲事件到本地狀態；格式錯誤回傳 -1 */
static int apply_event(int code, const char *a)
{
    game_t *g = &CL.g;
    long long now = now_ms();
    int s, x, y, o, t, rg, n, k;
    char ch;

    switch (code) {
    case S_MOVED:
        if (sscanf(a, "%d %d %d", &s, &x, &y) != 3 || !valid_seat(s) || !game_in_bounds(x, y)) return -1;
        g->pl[s].x = x;
        g->pl[s].y = y;
        break;

    case S_BOMB:
        if (sscanf(a, "%d %d %d %d %d", &x, &y, &o, &t, &rg) != 5 || !game_in_bounds(x, y)) return -1;
        if ((k = game_free_bomb_slot(g)) < 0) return -1;
        g->bombs[k] = (bomb_t){1, x, y, o, t, rg};
        CL.bomb_deadline[k] = now + (long long)t * TICK_MS;
        break;

    case S_EXPLODE: {
        if (sscanf(a, "%d %d %d%n", &x, &y, &n, &k) != 3) return -1;
        int b = game_bomb_at(g, x, y);
        if (b >= 0) g->bombs[b].used = 0;
        const char *p = a + k;
        for (int i = 0; i < n; i++) {
            int cx, cy, m;
            if (sscanf(p, " %d,%d%n", &cx, &cy, &m) != 2 || !game_in_bounds(cx, cy)) return -1;
            CL.flame_until[cy][cx] = now + FLAME_TICKS * TICK_MS;
            p += m;
        }
        break;
    }

    case S_CELL:
        if (sscanf(a, "%d %d %c", &x, &y, &ch) != 3 || !game_in_bounds(x, y)) return -1;
        g->map[y][x] = ch;
        break;

    case S_PICKUP:
        if (sscanf(a, "%d %d %d %c", &s, &x, &y, &ch) != 4 || !valid_seat(s) || !game_in_bounds(x, y)) return -1;
        game_apply_item(&g->pl[s], ch);
        g->map[y][x] = CELL_EMPTY;
        if (s == CL.my_seat)
            cl_log(&CL, "獲得道具：%s", ch == ITEM_BOMB ? "炸彈 +1" : ch == ITEM_RANGE ? "火力 +1" : "速度 +1");
        break;

    case S_TRAPPED:
        if (sscanf(a, "%d", &s) != 1 || !valid_seat(s)) return -1;
        g->pl[s].status = PS_TRAPPED;
        g->pl[s].trap_timer = TRAP_TICKS;
        CL.trap_until[s] = now + (long long)TRAP_TICKS * TICK_MS;
        if (s == CL.my_seat)
            cl_log(&CL, "\033[1;94m你被泡泡困住了！%s\033[0m",
                   CL.mode == MODE_TEAM ? "等隊友來救你（5 秒）" : "5 秒後破裂");
        else
            cl_log(&CL, "%s 被泡泡困住了", seat_name(&CL, s));
        break;

    case S_RESCUED:
        if (sscanf(a, "%d %d", &s, &o) != 2 || !valid_seat(s)) return -1;
        g->pl[s].status = PS_ALIVE;
        g->pl[s].trap_timer = 0;
        cl_log(&CL, "%s 救出了 %s！", seat_name(&CL, o), seat_name(&CL, s));
        break;

    case S_DIED: {
        int reason;
        if (sscanf(a, "%d %d %d", &s, &reason, &o) != 3 || !valid_seat(s)) return -1;
        g->pl[s].status = PS_DEAD;
        static const char *why[] = {"泡泡破裂", "被戳破", "認輸", "離開遊戲"};
        if (reason == DIE_POPPED && valid_seat(o))
            cl_log(&CL, "\033[91m%s 被 %s 戳破泡泡，出局！\033[0m", seat_name(&CL, s), seat_name(&CL, o));
        else
            cl_log(&CL, "\033[91m%s 出局（%s）\033[0m", seat_name(&CL, s),
                   (reason >= 0 && reason <= 3) ? why[reason] : "?");
        break;
    }

    case S_GAMEOVER: {
        int kind, id;
        if (sscanf(a, "%d %d", &kind, &id) != 2) return -1;
        if (kind == GO_PLAYER && valid_seat(id))
            snprintf(CL.result, sizeof CL.result, "%s", id == CL.my_seat ? "你贏了！" : "你輸了…");
        else if (kind == GO_TEAM)
            snprintf(CL.result, sizeof CL.result, "%s",
                     id == game_team_of(MODE_TEAM, CL.my_seat) ? "你的隊伍獲勝！" : "你的隊伍落敗…");
        else
            snprintf(CL.result, sizeof CL.result, "平手");
        if (kind == GO_PLAYER && valid_seat(id))
            cl_log(&CL, "\033[1;93m遊戲結束：%s 獲勝！\033[0m", seat_name(&CL, id));
        else if (kind == GO_TEAM)
            cl_log(&CL, "\033[1;93m遊戲結束：%s 隊獲勝！\033[0m", id == 0 ? "A" : "B");
        else
            cl_log(&CL, "\033[1;93m遊戲結束：平手\033[0m");
        cl_log(&CL, "5 秒後回到房間…");
        CL.st = CS_ENDED;
        CL.chat_mode = CL.confirm = 0;
        CL.input_len = 0;
        CL.input[0] = '\0';
        break;
    }
    default:
        return -1;
    }
    return 0;
}

static int is_game_event(int code)
{
    return (code >= S_MOVED && code <= S_DIED) || code == S_GAMEOVER;
}

/* ---------------------------------------------------- Server 訊息處理 */

static void parse_roster(const char *a)
{
    int mode, n, k;
    if (sscanf(a, "%d %d%n", &mode, &n, &k) != 2) return;
    CL.mode = mode;
    memset(CL.roster, 0, sizeof CL.roster);
    const char *p = a + k;
    for (int i = 0; i < n; i++) {
        int seat, ready, m;
        char nick[NICK_MAX + 1];
        if (sscanf(p, " %d:%24[^:]:%d%n", &seat, nick, &ready, &m) != 3) break;
        if (valid_seat(seat)) {
            CL.roster[seat].used = 1;
            CL.roster[seat].ready = ready;
            snprintf(CL.roster[seat].nick, sizeof CL.roster[seat].nick, "%s", nick);
        }
        p += m;
    }
    CL.dirty = 1;
}

static void parse_roomlist(const char *a)
{
    int n, k;
    CL.nrooms = 0;
    if (sscanf(a, "%d%n", &n, &k) != 1) return;
    const char *p = a + k;
    for (int i = 0; i < n && CL.nrooms < MAX_ROOMS; i++) {
        roominfo_t *r = &CL.rooms[CL.nrooms];
        int m;
        if (sscanf(p, " %d:%d:%d:%d%n", &r->id, &r->mode, &r->count, &r->state, &m) != 4) break;
        CL.nrooms++;
        p += m;
    }
    CL.dirty = 1;
}

static void go_lobby(void)
{
    CL.st = CS_LOBBY;
    CL.countdown = 0;
    memset(CL.roster, 0, sizeof CL.roster);
    csend("%d", C_LIST);
}

static void on_server_line(char *line)
{
    char *save = NULL;
    char *tok = strtok_r(line, " ", &save);
    if (!tok) return;
    int code = atoi(tok);
    const char *a = save ? save : "";
    int x, y;

    if (code != S_PONG) CL.dirty = 1;

    if (is_game_event(code)) {
        int seq, k;
        if (CL.st != CS_GAME) return;
        if (!CL.synced || CL.resync_pending) return;   /* 等完整快照 */
        if (sscanf(a, "%d%n", &seq, &k) != 1) return;
        if (seq != CL.seq + 1) {
            char why[64];
            snprintf(why, sizeof why, "事件序號不連續（預期 %d，收到 %d）", CL.seq + 1, seq);
            request_resync(why);
            return;
        }
        CL.seq = seq;
        if (apply_event(code, a + k) < 0) request_resync("事件格式錯誤");
        return;
    }

    switch (code) {
    case S_WELCOME:
        if (CL.st != CS_HELLO_SENT) break;
        CL.my_id = atoi(a);
        cl_log(&CL, "歡迎，%s！輸入 /create 建立房間，或 /join 編號 加入房間", CL.nick);
        go_lobby();
        break;

    case S_ROOMLIST:
        parse_roomlist(a);
        break;

    case S_JOINED:
        if (sscanf(a, "%d %d %d", &CL.room_id, &CL.my_seat, &CL.mode) != 3) break;
        CL.st = CS_ROOM;
        cl_log(&CL, "已加入房間 #%d，你是第 %d 位加入的玩家（P%d）", CL.room_id, CL.my_seat + 1, CL.my_seat + 1);
        if (CL.my_seat == 0) cl_log(&CL, "等待其他玩家加入…人到齊後大家輸入 /ready");
        break;

    case S_ROSTER:
        parse_roster(a);
        break;

    case S_LEFT:
        if (sscanf(a, "%d %d", &x, &y) != 2) break;
        if (x == CL.my_seat && y == LEFT_NORMAL && CL.st >= CS_ROOM) {
            cl_log(&CL, "你已離開房間");
            go_lobby();
        } else {
            cl_log(&CL, "%s %s", seat_name(&CL, x), y == LEFT_DISCONNECT ? "斷線了" : "離開了房間");
        }
        break;

    case S_COUNTDOWN:
        CL.countdown = atoi(a);
        cl_log(&CL, "遊戲將在 %d 秒後開始…", CL.countdown);
        break;

    case S_CANCEL:
        CL.countdown = 0;
        cl_log(&CL, "有人取消準備或離開，倒數取消");
        break;

    case S_START:
        CL.st = CS_GAME;
        CL.countdown = 0;
        CL.synced = 0;
        CL.resync_pending = 0;
        CL.result[0] = '\0';
        CL.chat_mode = CL.confirm = 0;
        CL.queued_dir = 0;
        CL.input_len = 0;
        CL.input[0] = '\0';
        ui_clear();
        cl_log(&CL, "\033[1;92m遊戲開始！\033[0m WASD/方向鍵移動，空白鍵放炸彈");
        break;

    case S_SNAPSHOT: {
        int ticks;
        game_t g;
        if (game_snapshot_decode(&g, a, &x, &ticks) < 0) { cl_log(&CL, "快照格式錯誤"); break; }
        long long now = now_ms();
        CL.g = g;
        CL.seq = x;
        CL.synced = 1;
        CL.resync_pending = 0;
        CL.mode = g.mode;
        CL.game_end_ms = now + (long long)ticks * TICK_MS;
        memset(CL.flame_until, 0, sizeof CL.flame_until);
        for (int i = 0; i < MAX_BOMB_TOTAL; i++)
            CL.bomb_deadline[i] = now + (long long)g.bombs[i].timer * TICK_MS;
        for (int i = 0; i < MAX_PLAYERS; i++)
            CL.trap_until[i] = now + (long long)g.pl[i].trap_timer * TICK_MS;
        break;
    }

    case S_HASH: {
        unsigned h;
        if (CL.st != CS_GAME || !CL.synced || CL.resync_pending) break;
        if (sscanf(a, "%d %u", &x, &h) != 2) break;
        if (x == CL.seq && game_hash(&CL.g) != h) request_resync("本地狀態雜湊與伺服器不一致");
        break;
    }

    case S_BACK:
        CL.st = CS_ROOM;
        ui_clear();
        cl_log(&CL, "回到房間。輸入 /ready 準備下一局，/leave 離開");
        break;

    case S_CHAT:
        if (sscanf(a, "%d%n", &x, &y) != 1) break;
        cl_log(&CL, "\033[96m[%s]\033[0m%s", seat_name(&CL, x), a + y);
        break;

    case S_ERR:
        if (sscanf(a, "%d", &x) != 1) break;
        cl_log(&CL, "\033[91m[錯誤]\033[0m %s", err_text(x));
        if (CL.st == CS_HELLO_SENT) CL.st = CS_NICK;
        else if (CL.st == CS_JOINING) go_lobby();
        break;

    case S_SHUTDOWN:
        g_conn_ok = 0;
        core_quit("伺服器已關閉");
        break;

    case S_PONG:
        break;

    default:
        cl_log(&CL, "（忽略未知訊息 %d）", code);
    }
}

/* -------------------------------------------------------- 使用者輸入 */

void core_submit_line(const char *line)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", line);
    char *cmd = buf;
    while (*cmd == ' ') cmd++;

    if (!strcmp(cmd, "/quit")) { core_quit("已離開遊戲"); return; }

    switch (CL.st) {
    case CS_NICK:
        if (!valid_nick(cmd)) { cl_log(&CL, "暱稱需 1~24 位元組，不可含空白或冒號"); return; }
        snprintf(CL.nick, sizeof CL.nick, "%.24s", cmd);
        csend("%d %s", C_HELLO, CL.nick);
        CL.st = CS_HELLO_SENT;
        return;

    case CS_LOBBY:
        if (!strcmp(cmd, "/list")) { csend("%d", C_LIST); return; }
        if (!strncmp(cmd, "/create", 7)) {
            const char *m = cmd + 7;
            while (*m == ' ') m++;
            int mode = !strcmp(m, "team") ? MODE_TEAM : (!*m || !strcmp(m, "ffa")) ? MODE_FFA : -1;
            if (mode < 0) { cl_log(&CL, "用法：/create 或 /create team"); return; }
            csend("%d %d", C_CREATE, mode);
            CL.st = CS_JOINING;
            return;
        }
        if (!strncmp(cmd, "/join", 5)) {
            int id;
            if (sscanf(cmd + 5, "%d", &id) != 1) { cl_log(&CL, "用法：/join 房間編號"); return; }
            /* 本地先檢查（stateful client），避免送出必定失敗的請求 */
            roominfo_t *r = NULL;
            for (int i = 0; i < CL.nrooms; i++)
                if (CL.rooms[i].id == id) r = &CL.rooms[i];
            if (!r) { cl_log(&CL, "找不到房間 #%d（輸入 /list 更新列表）", id); return; }
            if (r->state != RSTATE_WAITING) { cl_log(&CL, "房間 #%d 正在遊戲中", id); return; }
            if (r->count >= MAX_PLAYERS) { cl_log(&CL, "房間 #%d 已滿", id); return; }
            csend("%d %d", C_JOIN, id);
            CL.st = CS_JOINING;
            return;
        }
        if (!strcmp(cmd, "/help") || !*cmd) {
            cl_log(&CL, "大廳指令：/list  /create  /create team  /join 編號  /quit");
            return;
        }
        cl_log(&CL, "大廳不能聊天，請先加入房間。輸入 /help 查看指令");
        return;

    case CS_ROOM:
    case CS_ENDED:
        if (!strcmp(cmd, "/leave")) { csend("%d", C_LEAVE); return; }
        if (!strcmp(cmd, "/ready") || !strcmp(cmd, "/unready")) {
            if (CL.st == CS_ENDED) { cl_log(&CL, "結算中，回到房間後才能準備"); return; }
            int want = !strcmp(cmd, "/ready");
            if (CL.roster[CL.my_seat].ready == want) { cl_log(&CL, want ? "你已經準備好了" : "你本來就還沒準備"); return; }
            csend("%d %d", C_READY, want);
            return;
        }
        if (!strcmp(cmd, "/help")) {
            cl_log(&CL, "房間指令：/ready  /unready  /leave  /quit；其他文字直接送出為聊天");
            return;
        }
        if (cmd[0] == '/') { cl_log(&CL, "未知指令，輸入 /help 查看"); return; }
        if (*cmd) csend("%d %s", C_CHAT, cmd);
        return;

    case CS_GAME:
        if (*cmd) csend("%d %s", C_CHAT, cmd);
        return;

    default:
        return;
    }
}

static void try_move(int dir)
{
    if (!CL.synced || CL.resync_pending) return;
    gplayer_t *p = &CL.g.pl[CL.my_seat];
    long long now = now_ms();
    if (p->status != PS_ALIVE) CL.queued_dir = 0;
    if (p->status == PS_TRAPPED) {
        if (now - CL.last_warn_ms > 1500) {
            cl_log(&CL, "你被困在泡泡裡，不能移動！");
            CL.last_warn_ms = now;
        }
        return;
    }
    /* 冷卻中：記住最後按的方向，冷卻結束時再送（輸入緩衝，快速連按不會掉鍵） */
    if (now - CL.last_move_ms < (long long)game_move_cd(p) * TICK_MS - 20) {
        CL.queued_dir = dir;
        return;
    }
    CL.queued_dir = 0;
    int nx, ny;
    if (!game_can_move(&CL.g, CL.my_seat, dir, &nx, &ny)) return;   /* 本地擋下：撞牆、炸彈、已出局 */
    csend("%d %c", C_MOVE, dir_to_char(dir));
    CL.last_move_ms = now;
}

static void try_bomb(void)
{
    if (!CL.synced || CL.resync_pending) return;
    gplayer_t *p = &CL.g.pl[CL.my_seat];
    if (p->status != PS_ALIVE) return;
    if (game_active_bombs(&CL.g, CL.my_seat) >= p->bombs_max) {
        long long now = now_ms();
        if (now - CL.last_warn_ms > 1000) {
            cl_log(&CL, "炸彈已用完（上限 %d），等它爆炸或撿道具", p->bombs_max);
            CL.last_warn_ms = now;
        }
        return;
    }
    if (!game_can_bomb(&CL.g, CL.my_seat)) return;
    csend("%d", C_BOMB);
}

int core_line_mode(void)
{
    return CL.st == CS_NICK || CL.st == CS_LOBBY || CL.st == CS_ROOM || CL.st == CS_ENDED
        || (CL.st == CS_GAME && CL.chat_mode);
}

void core_line_key(unsigned char c)
{
    if (c == '\r' || c == '\n') {
        CL.input[CL.input_len] = '\0';
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", CL.input);
        CL.input_len = 0;
        CL.input[0] = '\0';
        if (CL.st == CS_GAME) CL.chat_mode = 0;
        core_submit_line(tmp);
    } else if (c == 0x7f || c == 0x08) {
        /* 刪除一個完整的 UTF-8 字元 */
        while (CL.input_len > 0 && ((unsigned char)CL.input[CL.input_len - 1] & 0xC0) == 0x80) CL.input_len--;
        if (CL.input_len > 0) CL.input_len--;
        CL.input[CL.input_len] = '\0';
    } else if (c >= 0x20 && c != 0x7f && CL.input_len < (int)sizeof CL.input - 8) {
        CL.input[CL.input_len++] = (char)c;
        CL.input[CL.input_len] = '\0';
    }
    CL.dirty = 1;
}

void core_game_key(int key)
{
    if (CL.st != CS_GAME) return;
    if (CL.confirm) {
        CL.confirm = 0;
        if (key == 'y' || key == 'Y') csend("%d", C_SURRENDER);
        else cl_log(&CL, "取消認輸");
        return;
    }
    switch (key) {
    case 'w': case 'W': case KEY_UP: try_move(DIR_UP);    break;
    case 's': case 'S': case KEY_DOWN: try_move(DIR_DOWN);  break;
    case 'a': case 'A': case KEY_LEFT: try_move(DIR_LEFT);  break;
    case 'd': case 'D': case KEY_RIGHT: try_move(DIR_RIGHT); break;
    case ' ': try_bomb(); break;
    case '\r': case '\n': case 't': case 'T':
        CL.chat_mode = 1;
        break;
    case 'x': case 'X': {
        int st = CL.g.pl[CL.my_seat].status;
        if (st == PS_ALIVE || st == PS_TRAPPED) CL.confirm = 1;
        else cl_log(&CL, "你已經出局了，等待本局結束");
        break;
    }
    }
    CL.dirty = 1;
}

int core_connect(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

/* ------------------------------------------------------- 對外介面 */

void core_init(int sock, const char *host, const char *port)
{
    memset(&CL, 0, sizeof CL);
    CL.sock = sock;
    CL.st = CS_NICK;
    CL.last_recv_ms = CL.last_ping_ms = now_ms();
    lb_init(&g_lb);
    cl_log(&CL, "已連線到 %s:%s", host, port);
    cl_log(&CL, "請輸入暱稱後按 Enter（隨時可按 Ctrl+D 離開）");
}

int core_quitting(void) { return g_quit; }
const char *core_exit_reason(void) { return g_exit_reason; }

/* socket 可讀時呼叫：收資料、切行、逐行處理 */
void core_on_readable(void)
{
    char data[2048], line[MAX_LINE];
    ssize_t r = recv(CL.sock, data, sizeof data, 0);
    if (r <= 0) {
        if (r < 0 && errno == EINTR) return;
        g_conn_ok = 0;
        core_quit("與伺服器的連線中斷");
        return;
    }
    CL.last_recv_ms = now_ms();
    if (lb_feed(&g_lb, data, (size_t)r) < 0) { core_quit("伺服器送來過長的資料"); return; }
    while (!g_quit && lb_getline(&g_lb, line, sizeof line)) on_server_line(line);
}

/* 每一輪主迴圈呼叫：送出緩衝中的移動、心跳、逾時偵測 */
void core_periodic(void)
{
    long long now = now_ms();
    if (CL.queued_dir && CL.st == CS_GAME) try_move(CL.queued_dir);
    if (now - CL.last_ping_ms >= HEARTBEAT_SEC * 1000) {
        csend("%d", C_PING);
        CL.last_ping_ms = now;
    }
    if (now - CL.last_recv_ms > TIMEOUT_SEC * 1000) {
        g_conn_ok = 0;
        core_quit("伺服器超過 15 秒沒有回應，連線中斷");
    }
}

/* 遊戲中取消聊天輸入（Esc） */
void core_cancel_chat(void)
{
    if (CL.st != CS_GAME || !CL.chat_mode) return;
    CL.chat_mode = 0;
    CL.input_len = 0;
    CL.input[0] = '\0';
    CL.dirty = 1;
}

void core_finish(void)
{
    if (g_conn_ok) csend("%d", C_QUIT);
    close(CL.sock);
}
