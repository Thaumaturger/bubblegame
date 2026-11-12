#include <stdio.h>
#include <string.h>
#include "game.h"

const int DIR_DX[5] = {0, 0, 0, -1, 1};
const int DIR_DY[5] = {0, -1, 1, 0, 0};

int dir_from_char(char c)
{
    switch (c) {
    case 'U': return DIR_UP;
    case 'D': return DIR_DOWN;
    case 'L': return DIR_LEFT;
    case 'R': return DIR_RIGHT;
    }
    return DIR_NONE;
}

char dir_to_char(int d)
{
    static const char t[5] = {'?', 'U', 'D', 'L', 'R'};
    return (d >= 0 && d < 5) ? t[d] : '?';
}

/* 四個角落出生；團隊模式下 seat 0/2（左側）一隊、1/3（右側）一隊 */
void game_spawn_pos(int seat, int *x, int *y)
{
    static const int sx[MAX_PLAYERS] = {1, MAP_W - 2, 1, MAP_W - 2};
    static const int sy[MAX_PLAYERS] = {1, 1, MAP_H - 2, MAP_H - 2};
    *x = sx[seat];
    *y = sy[seat];
}

int game_team_of(int mode, int seat) { return mode == MODE_TEAM ? seat % 2 : seat; }

int game_in_bounds(int x, int y) { return x >= 0 && x < MAP_W && y >= 0 && y < MAP_H; }

int game_is_item(char c) { return c == ITEM_BOMB || c == ITEM_RANGE || c == ITEM_SPEED; }

int game_move_cd(const gplayer_t *p)
{
    int cd = BASE_MOVE_CD - p->speed;
    return cd < MIN_MOVE_CD ? MIN_MOVE_CD : cd;
}

int game_bomb_at(const game_t *g, int x, int y)
{
    for (int i = 0; i < MAX_BOMB_TOTAL; i++)
        if (g->bombs[i].used && g->bombs[i].x == x && g->bombs[i].y == y) return i;
    return -1;
}

int game_active_bombs(const game_t *g, int seat)
{
    int n = 0;
    for (int i = 0; i < MAX_BOMB_TOTAL; i++)
        if (g->bombs[i].used && g->bombs[i].owner == seat) n++;
    return n;
}

int game_free_bomb_slot(const game_t *g)
{
    for (int i = 0; i < MAX_BOMB_TOTAL; i++)
        if (!g->bombs[i].used) return i;
    return -1;
}

/* 移動規則：活著（沒被困住）、目標在地圖內、不是牆/方塊、沒有炸彈。
 * 玩家可以走出自己腳下的炸彈，但不能走進有炸彈的格子。 */
int game_can_move(const game_t *g, int seat, int dir, int *nx, int *ny)
{
    if (seat < 0 || seat >= MAX_PLAYERS || dir < DIR_UP || dir > DIR_RIGHT) return 0;
    const gplayer_t *p = &g->pl[seat];
    if (p->status != PS_ALIVE) return 0;
    int x = p->x + DIR_DX[dir], y = p->y + DIR_DY[dir];
    if (!game_in_bounds(x, y)) return 0;
    char c = g->map[y][x];
    if (c == CELL_WALL || c == CELL_BLOCK) return 0;
    if (game_bomb_at(g, x, y) >= 0) return 0;
    *nx = x;
    *ny = y;
    return 1;
}

/* 放炸彈規則：活著、場上自己的炸彈數未達上限、腳下沒有炸彈 */
int game_can_bomb(const game_t *g, int seat)
{
    if (seat < 0 || seat >= MAX_PLAYERS) return 0;
    const gplayer_t *p = &g->pl[seat];
    return p->status == PS_ALIVE
        && game_active_bombs(g, seat) < p->bombs_max
        && game_bomb_at(g, p->x, p->y) < 0
        && game_free_bomb_slot(g) >= 0;
}

void game_apply_item(gplayer_t *p, char item)
{
    switch (item) {
    case ITEM_BOMB:  if (p->bombs_max < MAX_BOMBS) p->bombs_max++; break;
    case ITEM_RANGE: if (p->range < MAX_RANGE) p->range++;         break;
    case ITEM_SPEED: if (p->speed < MAX_SPEED) p->speed++;         break;
    }
}

/* FNV-1a 雜湊：涵蓋地圖、炸彈位置、所有玩家的狀態。
 * 不含炸彈倒數與火焰（純顯示用，Client 以本地時間推算）。 */
uint32_t game_hash(const game_t *g)
{
    uint32_t h = 2166136261u;
#define MIX(v) do { uint32_t _v = (uint32_t)(v); \
        for (int _k = 0; _k < 4; _k++) { h ^= (_v >> (_k * 8)) & 0xffu; h *= 16777619u; } } while (0)
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            MIX(g->map[y][x]);
            MIX(game_bomb_at(g, x, y) >= 0);
        }
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const gplayer_t *p = &g->pl[i];
        MIX(p->status); MIX(p->x); MIX(p->y);
        MIX(p->bombs_max); MIX(p->range); MIX(p->speed);
    }
#undef MIX
    return h;
}

int game_snapshot_encode(const game_t *g, int seq, int ticks_left, char *out, size_t n)
{
    int off = snprintf(out, n, "%d %d %d ", seq, ticks_left, g->mode);
    if (off < 0 || (size_t)off + MAP_W * MAP_H + 1 >= n) return -1;
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) out[off++] = g->map[y][x];
    out[off] = '\0';

    for (int i = 0; i < MAX_PLAYERS; i++) {
        const gplayer_t *p = &g->pl[i];
        off += snprintf(out + off, n - (size_t)off, " %d,%d,%d,%d,%d,%d,%d",
                        p->status, p->x, p->y, p->bombs_max, p->range, p->speed, p->trap_timer);
        if ((size_t)off >= n) return -1;
    }
    int nb = 0;
    for (int i = 0; i < MAX_BOMB_TOTAL; i++) nb += g->bombs[i].used;
    off += snprintf(out + off, n - (size_t)off, " %d", nb);
    if ((size_t)off >= n) return -1;
    for (int i = 0; i < MAX_BOMB_TOTAL; i++) {
        const bomb_t *b = &g->bombs[i];
        if (!b->used) continue;
        off += snprintf(out + off, n - (size_t)off, " %d,%d,%d,%d,%d",
                        b->x, b->y, b->owner, b->timer, b->range);
        if ((size_t)off >= n) return -1;
    }
    return off;
}

int game_snapshot_decode(game_t *g, const char *s, int *seq, int *ticks_left)
{
    char fmt[32], map[MAP_W * MAP_H + 1];
    int off = 0, n = 0;
    game_t t;
    memset(&t, 0, sizeof t);

    snprintf(fmt, sizeof fmt, "%%d %%d %%d %%%ds%%n", MAP_W * MAP_H);
    if (sscanf(s, fmt, seq, ticks_left, &t.mode, map, &n) != 4) return -1;
    if (strlen(map) != MAP_W * MAP_H) return -1;
    if (t.mode != MODE_FFA && t.mode != MODE_TEAM) return -1;
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) t.map[y][x] = map[y * MAP_W + x];
    off += n;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        gplayer_t *p = &t.pl[i];
        if (sscanf(s + off, " %d,%d,%d,%d,%d,%d,%d%n", &p->status, &p->x, &p->y,
                   &p->bombs_max, &p->range, &p->speed, &p->trap_timer, &n) != 7) return -1;
        if (p->status < PS_EMPTY || p->status > PS_DEAD) return -1;
        if (p->status != PS_EMPTY && !game_in_bounds(p->x, p->y)) return -1;
        off += n;
    }
    int nb;
    if (sscanf(s + off, " %d%n", &nb, &n) != 1 || nb < 0 || nb > MAX_BOMB_TOTAL) return -1;
    off += n;
    for (int j = 0; j < nb; j++) {
        bomb_t *b = &t.bombs[j];
        if (sscanf(s + off, " %d,%d,%d,%d,%d%n", &b->x, &b->y, &b->owner, &b->timer, &b->range, &n) != 5)
            return -1;
        if (!game_in_bounds(b->x, b->y)) return -1;
        b->used = 1;
        off += n;
    }
    *g = t;
    return 0;
}
