/*
 * room.c — 房間執行緒、狀態機（狀態 × 事件 查表）、遊戲模擬
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "server.h"
#include "net.h"

room_t g_rooms[MAX_ROOMS];

typedef void (*handler_t)(room_t *r, event_t *e);

/* ======================================================================
 *  工具函式
 * ====================================================================== */

static int seat_of(room_t *r, client_t *c)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (r->seat[i] == c) return i;
    return -1;
}

static int player_count(room_t *r)
{
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) n += r->seat[i] != NULL;
    return n;
}

static void set_state(room_t *r, room_state_t s)
{
    pthread_mutex_lock(&r->mtx);
    r->state = s;
    r->nplayers = player_count(r);
    pthread_mutex_unlock(&r->mtx);
}

static void publish_count(room_t *r)
{
    pthread_mutex_lock(&r->mtx);
    r->nplayers = player_count(r);
    pthread_mutex_unlock(&r->mtx);
}

/* 廣播給房內所有玩家 */
static void bcast(room_t *r, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bcast(room_t *r, const char *fmt, ...)
{
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (r->seat[i]) cli_send(r->seat[i], "%s", buf);
}

/* 廣播遊戲事件：自動加上遞增序號，讓 Client 偵測漏收或不一致 */
static void emit(room_t *r, int code, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void emit(room_t *r, int code, const char *fmt, ...)
{
    char body[MAX_LINE - 32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    r->seq++;
    bcast(r, "%d %d %s", code, r->seq, body);
}

static void send_roster(room_t *r)
{
    char buf[MAX_LINE];
    int off = snprintf(buf, sizeof buf, "%d %d %d", S_ROSTER, r->mode, player_count(r));
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (r->seat[i])
            off += snprintf(buf + off, sizeof buf - (size_t)off, " %d:%s:%d",
                            i, r->seat[i]->nick, r->ready[i]);
    bcast(r, "%s", buf);
}

static void send_snapshot(room_t *r, client_t *c)
{
    char buf[MAX_LINE];
    if (game_snapshot_encode(&r->g, r->seq, r->ticks_left, buf, sizeof buf) < 0) return;
    cli_send(c, "%d %s", S_SNAPSHOT, buf);
}

static void send_err(client_t *c, int code, const char *msg)
{
    cli_send(c, "%d %d %s", S_ERR, code, msg);
}

static void release_client(client_t *c)
{
    pthread_mutex_lock(&c->mtx);
    c->room = NULL;
    c->released = 1;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mtx);
}

/* ======================================================================
 *  遊戲邏輯
 * ====================================================================== */

static void gen_map(room_t *r)
{
    game_t *g = &r->g;
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            r->hidden[y][x] = CELL_EMPTY;
            if (x == 0 || y == 0 || x == MAP_W - 1 || y == MAP_H - 1 || (x % 2 == 0 && y % 2 == 0))
                g->map[y][x] = CELL_WALL;
            else
                g->map[y][x] = (rand_r(&r->rng) % 100 < 70) ? CELL_BLOCK : CELL_EMPTY;
        }
    /* 出生點與相鄰兩格淨空，避免一開始就被困死 */
    for (int s = 0; s < MAX_PLAYERS; s++) {
        int sx, sy;
        game_spawn_pos(s, &sx, &sy);
        for (int d = 0; d <= 4; d++) {
            int x = sx + DIR_DX[d], y = sy + DIR_DY[d];
            if (game_in_bounds(x, y) && g->map[y][x] == CELL_BLOCK) g->map[y][x] = CELL_EMPTY;
        }
    }
    /* 方塊底下藏道具：35% 機率 */
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            if (g->map[y][x] != CELL_BLOCK) continue;
            int roll = rand_r(&r->rng) % 100;
            if (roll < 14)      r->hidden[y][x] = ITEM_BOMB;
            else if (roll < 28) r->hidden[y][x] = ITEM_RANGE;
            else if (roll < 35) r->hidden[y][x] = ITEM_SPEED;
        }
}

static void trap_player(room_t *r, int i)
{
    r->g.pl[i].status = PS_TRAPPED;
    r->g.pl[i].trap_timer = TRAP_TICKS;
    r->pending[i] = 0;
    emit(r, S_TRAPPED, "%d", i);
}

static void kill_player(room_t *r, int i, int reason, int by)
{
    r->g.pl[i].status = PS_DEAD;
    r->pending[i] = 0;
    emit(r, S_DIED, "%d %d %d", i, reason, by);
}

static void game_over(room_t *r, int kind, int id)
{
    emit(r, S_GAMEOVER, "%d %d", kind, id);
    r->timer = ENDED_TICKS;
    set_state(r, RS_ENDED);
    srv_log("room %d: game over kind=%d id=%d", r->id, kind, id);
}

/* 回傳 1 表示遊戲結束 */
static int check_win(room_t *r)
{
    int alive = 0, last = -1, team_alive[2] = {0, 0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        int st = r->g.pl[i].status;
        if (st == PS_ALIVE || st == PS_TRAPPED) {
            alive++;
            last = i;
            team_alive[i % 2]++;
        }
    }
    if (r->mode == MODE_FFA) {
        if (alive <= 1) { game_over(r, alive ? GO_PLAYER : GO_DRAW, last); return 1; }
    } else {
        int t0 = team_alive[0] > 0, t1 = team_alive[1] > 0;
        if (!(t0 && t1)) { game_over(r, (t0 || t1) ? GO_TEAM : GO_DRAW, t0 ? 0 : (t1 ? 1 : -1)); return 1; }
    }
    if (r->ticks_left <= 0) { game_over(r, GO_DRAW, -1); return 1; }
    return 0;
}

/* 引爆炸彈（含連鎖）。火焰往四方延伸 range 格：
 * 遇牆停止；遇方塊則摧毀並停止（露出道具）；遇道具則燒毀；遇炸彈則連鎖引爆。 */
static void explode_chain(room_t *r, int first)
{
    game_t *g = &r->g;
    int queue[MAX_BOMB_TOTAL], qh = 0, qt = 0;
    int queued[MAX_BOMB_TOTAL] = {0};
    char revealed[MAP_H][MAP_W];
    memset(revealed, 0, sizeof revealed);

    queue[qt++] = first;
    queued[first] = 1;
    while (qh < qt) {
        bomb_t *b = &g->bombs[queue[qh++]];
        if (!b->used) continue;
        int bx = b->x, by = b->y, range = b->range;
        b->used = 0;

        char cells[MAX_LINE / 2];
        int off = 0, nc = 0;
        r->flame[by][bx] = FLAME_TICKS;
        off += snprintf(cells + off, sizeof cells - (size_t)off, " %d,%d", bx, by);
        nc++;

        for (int d = DIR_UP; d <= DIR_RIGHT; d++) {
            for (int k = 1; k <= range; k++) {
                int cx = bx + DIR_DX[d] * k, cy = by + DIR_DY[d] * k;
                if (!game_in_bounds(cx, cy)) break;
                char c = g->map[cy][cx];
                if (c == CELL_WALL) break;
                r->flame[cy][cx] = FLAME_TICKS;
                off += snprintf(cells + off, sizeof cells - (size_t)off, " %d,%d", cx, cy);
                nc++;
                if (c == CELL_BLOCK) {
                    char it = r->hidden[cy][cx];
                    g->map[cy][cx] = it;
                    r->hidden[cy][cx] = CELL_EMPTY;
                    revealed[cy][cx] = 1;
                    emit(r, S_CELL, "%d %d %c", cx, cy, it);
                    break;
                }
                if (game_is_item(c) && !revealed[cy][cx]) {
                    g->map[cy][cx] = CELL_EMPTY;
                    emit(r, S_CELL, "%d %d %c", cx, cy, CELL_EMPTY);
                }
                int ob = game_bomb_at(g, cx, cy);
                if (ob >= 0 && !queued[ob]) {
                    queued[ob] = 1;
                    queue[qt++] = ob;
                }
            }
        }
        emit(r, S_EXPLODE, "%d %d %d%s", bx, by, nc, cells);
    }
}

static void start_game(room_t *r)
{
    game_t *g = &r->g;
    memset(g, 0, sizeof *g);
    memset(r->flame, 0, sizeof r->flame);
    memset(r->pending, 0, sizeof r->pending);
    g->mode = r->mode;
    gen_map(r);
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gplayer_t *p = &g->pl[i];
        if (!r->seat[i]) { p->status = PS_EMPTY; continue; }
        p->status = PS_ALIVE;
        game_spawn_pos(i, &p->x, &p->y);
        p->bombs_max = 1;
        p->range = 1;
        p->speed = 0;
    }
    r->seq = 0;
    r->ticks_left = GAME_TICKS;
    set_state(r, RS_PLAYING);
    bcast(r, "%d", S_START);
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (r->seat[i]) send_snapshot(r, r->seat[i]);
    srv_log("room %d: game started (%d players, mode=%d)", r->id, player_count(r), r->mode);
}

/* 每 tick 的遊戲模擬（Server 為唯一權威） */
static void sim_tick(room_t *r)
{
    game_t *g = &r->g;

    /* 1. 移動：冷卻結束才執行緩衝中的方向 */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gplayer_t *p = &g->pl[i];
        if (p->status != PS_ALIVE) continue;
        if (p->move_cd > 0) p->move_cd--;
        if (r->pending[i] && p->move_cd == 0) {
            int nx, ny, d = r->pending[i];
            r->pending[i] = 0;
            if (game_can_move(g, i, d, &nx, &ny)) {
                p->x = nx;
                p->y = ny;
                p->move_cd = game_move_cd(p);
                emit(r, S_MOVED, "%d %d %d", i, nx, ny);
                char c = g->map[ny][nx];
                if (game_is_item(c)) {
                    game_apply_item(p, c);
                    g->map[ny][nx] = CELL_EMPTY;
                    emit(r, S_PICKUP, "%d %d %d %c", i, nx, ny, c);
                }
            }
        }
    }

    /* 2. 火焰消退 */
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            if (r->flame[y][x]) r->flame[y][x]--;

    /* 3. 炸彈倒數與爆炸 */
    for (int b = 0; b < MAX_BOMB_TOTAL; b++)
        if (g->bombs[b].used && --g->bombs[b].timer <= 0) explode_chain(r, b);

    /* 4. 站在火焰上的玩家被泡泡困住 */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gplayer_t *p = &g->pl[i];
        if (p->status == PS_ALIVE && r->flame[p->y][p->x]) trap_player(r, i);
    }

    /* 5. 泡泡計時到 → 死亡 */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gplayer_t *p = &g->pl[i];
        if (p->status == PS_TRAPPED && --p->trap_timer <= 0) kill_player(r, i, DIE_BUBBLE, -1);
    }

    /* 6. 接觸：隊友碰到 → 救出；敵人碰到 → 戳破（死亡） */
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gplayer_t *t = &g->pl[i];
        if (t->status != PS_TRAPPED) continue;
        for (int j = 0; j < MAX_PLAYERS; j++) {
            gplayer_t *o = &g->pl[j];
            if (j == i || o->status != PS_ALIVE || o->x != t->x || o->y != t->y) continue;
            if (game_team_of(r->mode, i) == game_team_of(r->mode, j)) {
                t->status = PS_ALIVE;
                t->trap_timer = 0;
                t->move_cd = 0;
                emit(r, S_RESCUED, "%d %d", i, j);
            } else {
                kill_player(r, i, DIE_POPPED, j);
            }
            break;
        }
    }

    r->ticks_left--;
    if (r->tick % TICKS_PER_SEC == 0)
        bcast(r, "%d %d %u", S_HASH, r->seq, (unsigned)game_hash(g));
    check_win(r);
}

static void room_reset_to_waiting(room_t *r)
{
    for (int i = 0; i < MAX_PLAYERS; i++) r->ready[i] = 0;
    set_state(r, RS_WAITING);
    bcast(r, "%d", S_BACK);
    send_roster(r);
}

static void try_start_countdown(room_t *r)
{
    int n = 0, teams[2] = {0, 0};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!r->seat[i]) continue;
        if (!r->ready[i]) return;
        n++;
        teams[i % 2]++;
    }
    if (n < 2) return;
    if (r->mode == MODE_TEAM && (!teams[0] || !teams[1])) {
        bcast(r, "%d %d %s", S_ERR, E_STATE, "need_both_teams");
        return;
    }
    r->timer = COUNTDOWN_TICKS;
    set_state(r, RS_COUNTDOWN);
    bcast(r, "%d %d", S_COUNTDOWN, COUNTDOWN_TICKS / TICKS_PER_SEC);
}

static void cancel_countdown(room_t *r)
{
    set_state(r, RS_WAITING);
    bcast(r, "%d", S_CANCEL);
}

static void room_tick(room_t *r)
{
    r->tick++;
    switch (r->state) {
    case RS_COUNTDOWN:
        if (--r->timer <= 0) start_game(r);
        else if (r->timer % TICKS_PER_SEC == 0) bcast(r, "%d %d", S_COUNTDOWN, r->timer / TICKS_PER_SEC);
        break;
    case RS_PLAYING:
        sim_tick(r);
        break;
    case RS_ENDED:
        if (--r->timer <= 0) room_reset_to_waiting(r);
        break;
    default:
        break;
    }
}

/* ======================================================================
 *  事件處理函式（對應狀態表的每一格）
 * ====================================================================== */

/* 注意：一旦 c->room 設為 NULL，client thread 斷線時就不會再等 room 放手，
 * 會直接釋放 client。因此「改狀態 + 送訊息」必須在持有 c->mtx 時一起完成，
 * 否則可能對已釋放的 client 送資料（ThreadSanitizer 抓到的 use-after-free）。 */
static void join_reject(client_t *c, int code, const char *msg)
{
    pthread_mutex_lock(&c->mtx);
    c->room = NULL;
    c->state = SS_LOBBY;
    send_err(c, code, msg);
    pthread_mutex_unlock(&c->mtx);
}

static void h_ignore(room_t *r, event_t *e) { (void)r; (void)e; }

/* 先確認還在房內：client 送出 LEAVE 後、房間處理前，可能又排進其他事件；
 * 處理 LEAVE 後 client 若立刻斷線就會被釋放，不能再碰 e->cli。
 * （seat_of 只比較指標，不會讀取 client 內容） */
static void h_err_state(room_t *r, event_t *e)
{
    if (seat_of(r, e->cli) < 0) return;
    send_err(e->cli, E_STATE, "wrong_state");
}

static void h_join(room_t *r, event_t *e)
{
    client_t *c = e->cli;
    int s = 0;
    while (s < MAX_PLAYERS && r->seat[s]) s++;
    if (s == MAX_PLAYERS) { join_reject(c, E_FULL, "room_full"); return; }

    r->seat[s] = c;
    r->ready[s] = 0;
    pthread_mutex_lock(&c->mtx);
    c->state = SS_IN_ROOM;
    pthread_mutex_unlock(&c->mtx);
    publish_count(r);
    /* 告知玩家他是第幾位加入（seat 0 = 第一位，需等待其他玩家） */
    cli_send(c, "%d %d %d %d", S_JOINED, r->id, s, r->mode);
    send_roster(r);
    srv_log("room %d: %s joined as P%d", r->id, c->nick, s + 1);
}

static void h_join_busy(room_t *r, event_t *e)   { (void)r; join_reject(e->cli, E_BUSY, "room_busy"); }
static void h_join_closed(room_t *r, event_t *e) { (void)r; join_reject(e->cli, E_NO_ROOM, "no_room"); }

/* 離開與斷線共用：依目前狀態做不同處理 */
static void do_leave(room_t *r, client_t *c, int reason)
{
    int s = seat_of(r, c);
    if (s < 0) return;
    room_state_t st = r->state;

    if (st == RS_PLAYING) {
        int ps = r->g.pl[s].status;
        if (ps == PS_ALIVE || ps == PS_TRAPPED) kill_player(r, s, DIE_LEFT, -1);
    }
    r->seat[s] = NULL;
    r->ready[s] = 0;
    r->pending[s] = 0;
    int remain = player_count(r);
    if (remain == 0) set_state(r, RS_CLOSED);   /* 先更新大廳看得到的狀態，再通知 */
    else publish_count(r);
    /* 在 c->room = NULL 之前記錄：之後 client thread 可能隨時釋放 c */
    srv_log("room %d: P%d %s (%s)", r->id, s + 1, c->nick, reason ? "disconnected" : "left");
    if (reason == LEFT_NORMAL) {
        pthread_mutex_lock(&c->mtx);          /* 同 join_reject 的說明 */
        c->room = NULL;
        c->state = SS_LOBBY;
        cli_send(c, "%d %d %d", S_LEFT, s, LEFT_NORMAL);
        pthread_mutex_unlock(&c->mtx);
    }
    bcast(r, "%d %d %d", S_LEFT, s, reason);

    if (remain == 0) {
        srv_log("room %d: closed", r->id);
        return;
    }
    send_roster(r);
    if (st == RS_COUNTDOWN) cancel_countdown(r);
    else if (st == RS_PLAYING) check_win(r);
}

static void h_leave(room_t *r, event_t *e) { do_leave(r, e->cli, LEFT_NORMAL); }

static void h_disconnect(room_t *r, event_t *e)
{
    do_leave(r, e->cli, LEFT_DISCONNECT);
    release_client(e->cli);   /* 之後 room thread 不再碰這個 client */
}

static void h_ready(room_t *r, event_t *e)
{
    int s = seat_of(r, e->cli);
    if (s < 0) return;
    r->ready[s] = e->arg ? 1 : 0;
    send_roster(r);
    try_start_countdown(r);
}

static void h_ready_countdown(room_t *r, event_t *e)
{
    int s = seat_of(r, e->cli);
    if (s < 0 || e->arg) return;
    r->ready[s] = 0;
    send_roster(r);
    cancel_countdown(r);
}

static void h_move(room_t *r, event_t *e)
{
    int s = seat_of(r, e->cli);
    if (s < 0 || r->g.pl[s].status != PS_ALIVE) return;
    r->pending[s] = e->arg;
}

static void h_bomb(room_t *r, event_t *e)
{
    int s = seat_of(r, e->cli);
    if (s < 0) return;
    game_t *g = &r->g;
    if (!game_can_bomb(g, s)) { send_err(e->cli, E_ACTION, "cannot_bomb"); return; }
    gplayer_t *p = &g->pl[s];
    bomb_t *b = &g->bombs[game_free_bomb_slot(g)];
    b->used = 1;
    b->x = p->x;
    b->y = p->y;
    b->owner = s;
    b->timer = BOMB_TICKS;
    b->range = p->range;
    emit(r, S_BOMB, "%d %d %d %d %d", b->x, b->y, s, b->timer, b->range);
}

static void h_chat(room_t *r, event_t *e)
{
    int s = seat_of(r, e->cli);
    if (s < 0 || !e->text[0]) return;
    bcast(r, "%d %d %s", S_CHAT, s, e->text);
}

static void h_surrender(room_t *r, event_t *e)
{
    int s = seat_of(r, e->cli);
    if (s < 0) return;
    int ps = r->g.pl[s].status;
    if (ps != PS_ALIVE && ps != PS_TRAPPED) { send_err(e->cli, E_ACTION, "already_out"); return; }
    kill_player(r, s, DIE_SURRENDER, -1);
    check_win(r);
}

static void h_resync(room_t *r, event_t *e)
{
    if (seat_of(r, e->cli) < 0) return;
    srv_log("room %d: resync requested by %s", r->id, e->cli->nick);
    send_snapshot(r, e->cli);
}

/* ======================================================================
 *  狀態 × 事件 對照表（與報告中的狀態表一一對應）
 *  每一格都有處理函式，保證任何組合都不會 block / hang。
 * ====================================================================== */
static const handler_t TABLE[RS_MAX][EV_MAX] = {
    [RS_CLOSED] = {
        [EV_JOIN] = h_join_closed, [EV_LEAVE] = h_ignore,   [EV_READY] = h_ignore,
        [EV_MOVE] = h_ignore,      [EV_BOMB] = h_ignore,    [EV_CHAT] = h_ignore,
        [EV_SURRENDER] = h_ignore, [EV_RESYNC] = h_ignore,  [EV_DISCONNECT] = h_disconnect },
    [RS_WAITING] = {
        [EV_JOIN] = h_join,          [EV_LEAVE] = h_leave,      [EV_READY] = h_ready,
        [EV_MOVE] = h_err_state,     [EV_BOMB] = h_err_state,   [EV_CHAT] = h_chat,
        [EV_SURRENDER] = h_err_state,[EV_RESYNC] = h_err_state, [EV_DISCONNECT] = h_disconnect },
    [RS_COUNTDOWN] = {
        [EV_JOIN] = h_join_busy,     [EV_LEAVE] = h_leave,      [EV_READY] = h_ready_countdown,
        [EV_MOVE] = h_err_state,     [EV_BOMB] = h_err_state,   [EV_CHAT] = h_chat,
        [EV_SURRENDER] = h_err_state,[EV_RESYNC] = h_err_state, [EV_DISCONNECT] = h_disconnect },
    [RS_PLAYING] = {
        [EV_JOIN] = h_join_busy,     [EV_LEAVE] = h_leave,      [EV_READY] = h_err_state,
        [EV_MOVE] = h_move,          [EV_BOMB] = h_bomb,        [EV_CHAT] = h_chat,
        [EV_SURRENDER] = h_surrender,[EV_RESYNC] = h_resync,    [EV_DISCONNECT] = h_disconnect },
    [RS_ENDED] = {
        [EV_JOIN] = h_join_busy,     [EV_LEAVE] = h_leave,      [EV_READY] = h_err_state,
        [EV_MOVE] = h_ignore,        [EV_BOMB] = h_ignore,      [EV_CHAT] = h_chat,
        [EV_SURRENDER] = h_err_state,[EV_RESYNC] = h_resync,    [EV_DISCONNECT] = h_disconnect },
};

static void dispatch(room_t *r, event_t *e)
{
    handler_t h = TABLE[r->state][e->type];
    if (h) h(r, e);
}

/* ======================================================================
 *  房間執行緒
 * ====================================================================== */

static int is_ticking(room_state_t s) { return s == RS_COUNTDOWN || s == RS_PLAYING || s == RS_ENDED; }

static void ts_add_ms(struct timespec *t, int ms)
{
    t->tv_nsec += (long)ms * 1000000L;
    while (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; t->tv_sec++; }
}

static int ts_cmp(const struct timespec *a, const struct timespec *b)
{
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec ? -1 : 1;
    if (a->tv_nsec != b->tv_nsec) return a->tv_nsec < b->tv_nsec ? -1 : 1;
    return 0;
}

static void *room_thread(void *arg)
{
    room_t *r = arg;
    struct timespec next, now;
    int ticking = 0;

    for (;;) {
        pthread_mutex_lock(&r->mtx);
        while (!r->qh) {
            if (is_ticking(r->state)) {
                if (!ticking) {
                    clock_gettime(CLOCK_MONOTONIC, &next);
                    ts_add_ms(&next, TICK_MS);
                    ticking = 1;
                }
                if (pthread_cond_timedwait(&r->cv, &r->mtx, &next) == ETIMEDOUT) break;
            } else {
                ticking = 0;
                pthread_cond_wait(&r->cv, &r->mtx);   /* 沒在計時就睡到有事件 */
            }
        }
        event_t *ev = r->qh;
        r->qh = r->qt = NULL;
        pthread_mutex_unlock(&r->mtx);

        /* 依序處理所有事件 */
        while (ev) {
            event_t *nx = ev->next;
            dispatch(r, ev);
            free(ev);
            ev = nx;
        }

        /* 時間到就推進遊戲（落後太多時最多補 5 個 tick） */
        if (is_ticking(r->state)) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!ticking) {
                next = now;
                ts_add_ms(&next, TICK_MS);
                ticking = 1;
            }
            for (int guard = 0; ts_cmp(&now, &next) >= 0 && is_ticking(r->state) && guard < 5; guard++) {
                room_tick(r);
                ts_add_ms(&next, TICK_MS);
            }
            if (ts_cmp(&now, &next) >= 0) {
                next = now;
                ts_add_ms(&next, TICK_MS);
            }
        } else {
            ticking = 0;
        }
    }
    return NULL;
}

/* ======================================================================
 *  對外介面（由 client thread 呼叫）
 * ====================================================================== */

void rooms_init(void)
{
    for (int i = 0; i < MAX_ROOMS; i++) {
        room_t *r = &g_rooms[i];
        memset(r, 0, sizeof *r);
        r->id = i + 1;
        r->state = RS_CLOSED;
        r->rng = (unsigned)time(NULL) ^ (unsigned)(i * 7919);
        pthread_mutex_init(&r->mtx, NULL);
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&r->cv, &ca);
        pthread_condattr_destroy(&ca);
        pthread_create(&r->th, NULL, room_thread, r);
        pthread_detach(r->th);
    }
}

/* 找一個關閉中的房間開放為等待狀態 */
room_t *room_claim(int mode)
{
    for (int i = 0; i < MAX_ROOMS; i++) {
        room_t *r = &g_rooms[i];
        pthread_mutex_lock(&r->mtx);
        if (r->state == RS_CLOSED && r->nplayers == 0) {
            r->state = RS_WAITING;
            r->mode = mode;
            pthread_mutex_unlock(&r->mtx);
            srv_log("room %d: created (mode=%d)", r->id, mode);
            return r;
        }
        pthread_mutex_unlock(&r->mtx);
    }
    return NULL;
}

room_t *room_find(int id)
{
    return (id >= 1 && id <= MAX_ROOMS) ? &g_rooms[id - 1] : NULL;
}

void room_post(room_t *r, event_t *e)
{
    e->next = NULL;
    pthread_mutex_lock(&r->mtx);
    if (r->qt) r->qt->next = e;
    else       r->qh = e;
    r->qt = e;
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->mtx);
}

void room_list_line(char *out, size_t n)
{
    char items[MAX_LINE / 2];
    int off = 0, cnt = 0;
    items[0] = '\0';
    for (int i = 0; i < MAX_ROOMS; i++) {
        room_t *r = &g_rooms[i];
        pthread_mutex_lock(&r->mtx);
        room_state_t st = r->state;
        int mode = r->mode, np = r->nplayers;
        pthread_mutex_unlock(&r->mtx);
        if (st == RS_CLOSED) continue;
        off += snprintf(items + off, sizeof items - (size_t)off, " %d:%d:%d:%d", r->id, mode, np, (int)st);
        cnt++;
    }
    snprintf(out, n, "%d %d%s", S_ROOMLIST, cnt, items);
}
