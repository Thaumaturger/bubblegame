/*
 * ui.c — 終端機畫面（ANSI escape sequence，不需額外函式庫）
 *
 * 版面（80x24）：
 *   第 1 列      標題列
 *   第 3~15 列   地圖（每格 2 字元寬）＋ 右側資訊欄 / 大廳、房間內容
 *   第 17~22 列  訊息紀錄
 *   第 23 列     輸入列
 *   第 24 列     按鍵說明
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "client.h"

static struct termios g_orig;
static int g_raw = 0;
static char g_out[1 << 16];
static size_t g_outn;

static const char *SEAT_FG[MAX_PLAYERS] = {"\033[1;91m", "\033[1;94m", "\033[1;92m", "\033[1;95m"};

static void put(const char *s)
{
    size_t n = strlen(s);
    if (g_outn + n < sizeof g_out) {
        memcpy(g_out + g_outn, s, n);
        g_outn += n;
    }
}

static void putf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void putf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(buf);
}

static void at(int row, int col) { putf("\033[%d;%dH", row, col); }
static void eol(void) { put("\033[0m\033[K"); }

static void flush_out(void)
{
    size_t off = 0;
    while (off < g_outn) {
        ssize_t w = write(STDOUT_FILENO, g_out + off, g_outn - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    g_outn = 0;
}

void ui_init(void)
{
    tcgetattr(STDIN_FILENO, &g_orig);
    struct termios t = g_orig;
    t.c_lflag &= ~(ICANON | ECHO | IEXTEN);   /* 保留 ISIG：Ctrl+C 仍會送出 SIGINT */
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    g_raw = 1;
    put("\033[?1049h\033[?25l\033[2J");   /* 切換到替代畫面、隱藏游標 */
    flush_out();
}

void ui_restore(void)
{
    if (!g_raw) return;
    g_outn = 0;
    put("\033[0m\033[?25h\033[?1049l");
    flush_out();
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
    g_raw = 0;
}

void ui_clear(void)
{
    put("\033[2J");
}

static const char *state_name(cstate_t s)
{
    switch (s) {
    case CS_NICK:       return "輸入暱稱";
    case CS_HELLO_SENT: return "登入中";
    case CS_LOBBY:      return "大廳";
    case CS_JOINING:    return "加入房間中";
    case CS_ROOM:       return "房間";
    case CS_GAME:       return "遊戲中";
    case CS_ENDED:      return "結算";
    }
    return "";
}

static const char *room_state_name(int s)
{
    switch (s) {
    case RSTATE_WAITING:   return "\033[92m等待中\033[0m";
    case RSTATE_COUNTDOWN: return "\033[93m倒數中\033[0m";
    case RSTATE_PLAYING:   return "\033[91m遊戲中\033[0m";
    case RSTATE_ENDED:     return "\033[91m結算中\033[0m";
    }
    return "?";
}

static const char *team_label(int mode, int seat)
{
    if (mode != MODE_TEAM) return "";
    return seat % 2 == 0 ? " \033[91m[A隊]\033[0m" : " \033[94m[B隊]\033[0m";
}

/* ------------------------------------------------------------ 地圖格子 */

static void draw_cell(const cl_t *cl, int x, int y, long long now)
{
    const game_t *g = &cl->g;
    /* 優先順序：玩家 > 火焰 > 炸彈 > 地形 / 道具 */
    int pl = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const gplayer_t *p = &g->pl[i];
        if ((p->status == PS_ALIVE || p->status == PS_TRAPPED) && p->x == x && p->y == y) {
            if (pl < 0 || p->status == PS_ALIVE) pl = i;
        }
    }
    if (pl >= 0) {
        if (g->pl[pl].status == PS_TRAPPED) putf("\033[44;97;1m(%d\033[0m", pl + 1);
        else putf("%sP%d\033[0m", SEAT_FG[pl], pl + 1);
        return;
    }
    if (cl->flame_until[y][x] > now) { put("\033[101;93m**\033[0m"); return; }
    int b = game_bomb_at(g, x, y);
    if (b >= 0) {
        long long left = cl->bomb_deadline[b] - now;
        int sec = (int)((left + 999) / 1000);
        if (sec < 1) sec = 1;
        if (sec > 9) sec = 9;
        putf("\033[41;97;1m@%d\033[0m", sec);
        return;
    }
    switch (g->map[y][x]) {
    case CELL_WALL:  put("\033[100m  \033[0m"); break;
    case CELL_BLOCK: put("\033[43;30m[]\033[0m"); break;
    case ITEM_BOMB:  put("\033[42;30;1mB+\033[0m"); break;
    case ITEM_RANGE: put("\033[46;30;1mR+\033[0m"); break;
    case ITEM_SPEED: put("\033[45;30;1mS+\033[0m"); break;
    default:         put("  "); break;
    }
}

static const char *status_text(int st)
{
    switch (st) {
    case PS_ALIVE:   return "\033[92m存活\033[0m";
    case PS_TRAPPED: return "\033[94;1m困住\033[0m";
    case PS_DEAD:    return "\033[90m出局\033[0m";
    }
    return "";
}

/* 右側資訊欄：回傳要畫在第 row 列的內容 */
static void panel_line(const cl_t *cl, int idx, long long now)
{
    const game_t *g = &cl->g;
    const gplayer_t *me = &g->pl[cl->my_seat];
    switch (idx) {
    case 0:
        if (cl->st == CS_ENDED) { putf("\033[1;93m%s\033[0m", cl->result); break; }
        {
            long long left = cl->game_end_ms - now;
            if (left < 0) left = 0;
            int s = (int)(left / 1000);
            putf("剩餘時間 \033[1m%d:%02d\033[0m   %s", s / 60, s % 60,
                 cl->mode == MODE_TEAM ? "團隊戰" : "個人戰");
        }
        break;
    case 2: case 3: case 4: case 5: {
        int i = idx - 2;
        if (g->pl[i].status == PS_EMPTY) break;
        const gplayer_t *p = &g->pl[i];
        putf("%sP%d\033[0m %-10.10s %s%s", SEAT_FG[i], i + 1, seat_name(cl, i), status_text(p->status),
             team_label(cl->mode, i));
        if (i == cl->my_seat) put(" \033[1m←你\033[0m");
        break;
    }
    case 7:
        putf("你的能力  炸彈 %d/%d  火力 %d  速度 %d",
             me->bombs_max - game_active_bombs(g, cl->my_seat), me->bombs_max, me->range, me->speed);
        break;
    case 9:  put("\033[43;30m[]\033[0m 方塊  \033[100m  \033[0m 牆  \033[41;97m@3\033[0m 炸彈  \033[101;93m**\033[0m 火焰"); break;
    case 10: put("\033[42;30mB+\033[0m 炸彈+1  \033[46;30mR+\033[0m 火力+1  \033[45;30mS+\033[0m 速度+1"); break;
    case 11: put("\033[44;97m(1\033[0m 被泡泡困住的玩家"); break;
    case 12:
        if (cl->st == CS_GAME && me->status == PS_TRAPPED)
            put("\033[1;94m你被困住了！\033[0m");
        else if (cl->st == CS_GAME && me->status == PS_DEAD)
            put("\033[90m你已出局，觀戰中…\033[0m");
        else if (!cl->synced || cl->resync_pending)
            put("\033[33m同步中…\033[0m");
        break;
    }
}

/* ---------------------------------------------------------- 各種畫面 */

static void draw_lobby(const cl_t *cl)
{
    at(3, 1); put("\033[1m房間列表\033[0m（/list 更新）"); eol();
    int row = 5;
    if (cl->nrooms == 0) {
        at(row++, 1); put("  目前沒有房間，輸入 /create 建立個人戰，/create team 建立團隊戰"); eol();
    }
    for (int i = 0; i < cl->nrooms; i++) {
        const roominfo_t *r = &cl->rooms[i];
        at(row++, 1);
        putf("  #%d   %s   %d/%d 人   %s", r->id, r->mode == MODE_TEAM ? "團隊戰" : "個人戰",
             r->count, MAX_PLAYERS, room_state_name(r->state));
        eol();
    }
    while (row <= 15) { at(row++, 1); eol(); }
}

static void draw_room(const cl_t *cl)
{
    at(3, 1);
    putf("\033[1m房間 #%d\033[0m   模式：%s", cl->room_id, cl->mode == MODE_TEAM ? "團隊戰（A隊 vs B隊）" : "個人戰");
    eol();
    at(4, 1); eol();
    int row = 5;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        at(row++, 1);
        if (cl->roster[i].used) {
            putf("  %sP%d\033[0m  %-16.16s %s%s", SEAT_FG[i], i + 1, cl->roster[i].nick,
                 cl->roster[i].ready ? "\033[92m[已準備]\033[0m" : "\033[90m[未準備]\033[0m",
                 team_label(cl->mode, i));
            if (i == cl->my_seat) put("  \033[1m←你\033[0m");
        } else {
            putf("  \033[90mP%d  （空位）\033[0m", i + 1);
        }
        eol();
    }
    at(row++, 1); eol();
    at(row++, 1);
    if (cl->countdown > 0) putf("\033[1;93m遊戲即將開始…\033[0m");
    else put("全員 /ready 且至少 2 人即開始（團隊戰需兩隊都有人）");
    eol();
    while (row <= 15) { at(row++, 1); eol(); }
}

static void draw_game(const cl_t *cl, long long now)
{
    for (int y = 0; y < MAP_H; y++) {
        at(3 + y, 1);
        for (int x = 0; x < MAP_W; x++) draw_cell(cl, x, y, now);
        put("  ");
        panel_line(cl, y, now);
        eol();
    }
}

void ui_render(cl_t *cl)
{
    long long now = now_ms();
    put("\033[H");

    /* 標題列 */
    at(1, 1);
    putf("\033[1;97;45m 泡泡大亂鬥 BubbleBrawl \033[0m  %s%s%s  [%s]",
         cl->nick[0] ? "玩家：" : "", cl->nick,
         (cl->st >= CS_ROOM) ? "" : "", state_name(cl->st));
    if (cl->st >= CS_ROOM) putf("  房間 #%d  P%d", cl->room_id, cl->my_seat + 1);
    eol();
    at(2, 1); eol();

    switch (cl->st) {
    case CS_LOBBY:
    case CS_JOINING:
        draw_lobby(cl);
        break;
    case CS_ROOM:
        draw_room(cl);
        break;
    case CS_GAME:
    case CS_ENDED:
        if (cl->synced) draw_game(cl, now);
        else for (int r = 3; r <= 15; r++) { at(r, 1); if (r == 8) put("  載入地圖中…"); eol(); }
        break;
    default:
        for (int r = 3; r <= 15; r++) {
            at(r, 1);
            if (r == 6) put("  \033[1m歡迎來到 泡泡大亂鬥！\033[0m");
            if (r == 8) put("  放水球炸彈困住對手，被困住的人碰到敵人就會被戳破。");
            if (r == 9) put("  團隊戰中，隊友碰到被困住的你可以把你救出來。");
            eol();
        }
    }

    /* 訊息紀錄 */
    at(16, 1); put("\033[90m────────────────────────────────────────────────────────────\033[0m"); eol();
    for (int i = 0; i < LOG_N; i++) {
        at(17 + i, 1);
        if (i < cl->log_n) putf(" %s", cl->log[i]);
        eol();
    }

    /* 輸入列 */
    at(23, 1);
    const char *prompt = NULL;
    switch (cl->st) {
    case CS_NICK:  prompt = "暱稱"; break;
    case CS_LOBBY: prompt = "大廳"; break;
    case CS_ROOM:
    case CS_ENDED: prompt = "房間"; break;
    case CS_GAME:  if (cl->chat_mode) prompt = "聊天"; break;
    default: break;
    }
    if (cl->st == CS_GAME && cl->confirm)
        put("\033[1;93m確定要認輸嗎？(y/n)\033[0m");
    else if (prompt)
        putf("\033[1m%s>\033[0m %s\033[7m \033[0m", prompt, cl->input);
    eol();

    /* 按鍵說明 */
    at(24, 1);
    put("\033[90m");
    switch (cl->st) {
    case CS_GAME:
        if (cl->chat_mode) put("Enter 送出  Esc 取消");
        else put("WASD/方向鍵 移動  空白鍵 放炸彈  T/Enter 聊天  X 認輸  Ctrl+D 離開");
        break;
    case CS_LOBBY: put("/list  /create  /create team  /join 編號  /quit  （Ctrl+D 離開）"); break;
    case CS_ROOM:  put("/ready  /unready  /leave  /quit  其他文字 = 聊天"); break;
    case CS_ENDED: put("結算中，5 秒後回到房間  /leave 離開"); break;
    default:       put("Enter 確認  Ctrl+D 離開"); break;
    }
    eol();

    flush_out();
}
