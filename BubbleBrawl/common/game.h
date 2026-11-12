/*
 * game.h — 共用遊戲規則模組
 *
 * Client 與 Server 編譯同一份規則：
 *   - Client 用來在送出前檢查動作是否合法（stateful client）
 *   - Server 用來做最終裁決（不信任 Client）
 * 兩邊判斷邏輯完全相同，可避免規則不一致。
 */
#ifndef GAME_H
#define GAME_H

#include <stddef.h>
#include <stdint.h>

#define MAP_W           15
#define MAP_H           13
#define MAX_PLAYERS     4
#define MAX_BOMB_TOTAL  32

#define TICK_MS         50                       /* Server 每 50ms 更新一次（20 Hz） */
#define TICKS_PER_SEC   (1000 / TICK_MS)
#define BOMB_TICKS      (3 * TICKS_PER_SEC)      /* 炸彈 3 秒後爆炸 */
#define FLAME_TICKS     (TICKS_PER_SEC / 2)      /* 火焰持續 0.5 秒 */
#define TRAP_TICKS      (5 * TICKS_PER_SEC)      /* 被泡泡困住 5 秒後死亡 */
#define GAME_TICKS      (180 * TICKS_PER_SEC)    /* 每局 3 分鐘 */
#define COUNTDOWN_TICKS (3 * TICKS_PER_SEC)
#define ENDED_TICKS     (5 * TICKS_PER_SEC)

#define BASE_MOVE_CD    5    /* 移動冷卻（tick），速度道具每級 -1 */
#define MIN_MOVE_CD     2
#define MAX_BOMBS       6
#define MAX_RANGE       6
#define MAX_SPEED       3

#define CELL_EMPTY  '.'
#define CELL_WALL   '#'      /* 不可破壞 */
#define CELL_BLOCK  '+'      /* 可破壞，可能藏道具 */
#define ITEM_BOMB   'b'      /* 炸彈數 +1 */
#define ITEM_RANGE  'r'      /* 火力 +1 */
#define ITEM_SPEED  's'      /* 速度 +1 */

enum { DIR_NONE = 0, DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
enum { PS_EMPTY = 0, PS_ALIVE, PS_TRAPPED, PS_DEAD };
enum { MODE_FFA = 0, MODE_TEAM = 1 };

extern const int DIR_DX[5];
extern const int DIR_DY[5];

typedef struct {
    int used, x, y, owner, timer, range;
} bomb_t;

typedef struct {
    int status;
    int x, y;
    int bombs_max, range, speed;
    int trap_timer;
    int move_cd;
} gplayer_t;

typedef struct {
    char      map[MAP_H][MAP_W];
    gplayer_t pl[MAX_PLAYERS];
    bomb_t    bombs[MAX_BOMB_TOTAL];
    int       mode;
} game_t;

int      dir_from_char(char c);
char     dir_to_char(int d);
void     game_spawn_pos(int seat, int *x, int *y);
int      game_team_of(int mode, int seat);
int      game_in_bounds(int x, int y);
int      game_is_item(char c);
int      game_move_cd(const gplayer_t *p);
int      game_bomb_at(const game_t *g, int x, int y);
int      game_active_bombs(const game_t *g, int seat);
int      game_free_bomb_slot(const game_t *g);
int      game_can_move(const game_t *g, int seat, int dir, int *nx, int *ny);
int      game_can_bomb(const game_t *g, int seat);
void     game_apply_item(gplayer_t *p, char item);
uint32_t game_hash(const game_t *g);

/* 完整狀態快照的編碼 / 解碼（S_SNAPSHOT 的內容） */
int game_snapshot_encode(const game_t *g, int seq, int ticks_left, char *out, size_t n);
int game_snapshot_decode(game_t *g, const char *s, int *seq, int *ticks_left);

#endif
