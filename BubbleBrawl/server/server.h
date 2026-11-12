/*
 * server.h — Server 端資料結構
 *
 * 執行緒模型：
 *   main thread      : accept() 新連線，為每個連線建立 client thread
 *   client thread    : recv() → 切行 → 解析；大廳指令直接處理，
 *                      房間內的指令轉成「事件」放進該房間的事件佇列
 *   room thread      : 每個房間一條，依「房間狀態 × 事件」查表處理，
 *                      並以 20Hz 推進遊戲（倒數、炸彈、火焰、泡泡計時）
 * 遊戲狀態只由 room thread 修改，因此遊戲邏輯本身不需要加鎖。
 */
#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <stddef.h>
#include "protocol.h"
#include "game.h"

/* 每個連線（session）的狀態 */
typedef enum { SS_CONNECTED, SS_LOBBY, SS_JOINING, SS_IN_ROOM } sess_state_t;

struct room;

typedef struct client {
    int  fd, id;
    char nick[NICK_MAX + 1];
    pthread_mutex_t send_mtx;   /* client thread 與 room thread 都會送資料給同一個 socket */
    pthread_mutex_t mtx;        /* 保護 state / room / released */
    pthread_cond_t  cv;
    sess_state_t    state;
    struct room    *room;
    int             released;   /* room thread 已不再引用此 client，可以釋放 */
    int             bad;        /* 錯誤訊息計數，過多則踢除 */
    struct client  *next;
} client_t;

typedef enum {
    EV_JOIN, EV_LEAVE, EV_READY, EV_MOVE, EV_BOMB,
    EV_CHAT, EV_SURRENDER, EV_RESYNC, EV_DISCONNECT, EV_MAX
} ev_type_t;

typedef struct event {
    ev_type_t     type;
    client_t     *cli;
    int           arg;
    char          text[CHAT_MAX + 8];
    struct event *next;
} event_t;

typedef enum {
    RS_CLOSED = RSTATE_CLOSED, RS_WAITING = RSTATE_WAITING, RS_COUNTDOWN = RSTATE_COUNTDOWN,
    RS_PLAYING = RSTATE_PLAYING, RS_ENDED = RSTATE_ENDED, RS_MAX
} room_state_t;

typedef struct room {
    int             id;
    pthread_t       th;
    pthread_mutex_t mtx;        /* 保護事件佇列與 state/mode/nplayers（供大廳查詢） */
    pthread_cond_t  cv;
    event_t        *qh, *qt;

    room_state_t state;
    int          mode;
    int          nplayers;

    /* 以下只有 room thread 會存取 */
    client_t     *seat[MAX_PLAYERS];
    int           ready[MAX_PLAYERS];
    int           pending[MAX_PLAYERS];   /* 緩衝一個移動方向，冷卻結束後執行 */
    game_t        g;
    char          hidden[MAP_H][MAP_W];   /* 方塊底下藏的道具（只有 Server 知道） */
    unsigned char flame[MAP_H][MAP_W];
    int           seq;                    /* 遊戲事件序號 */
    int           timer;                  /* 倒數 / 結算計時 */
    int           ticks_left;
    unsigned long tick;
    unsigned int  rng;
} room_t;

extern room_t g_rooms[MAX_ROOMS];

void    cli_send(client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void    srv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void    rooms_init(void);
room_t *room_claim(int mode);
room_t *room_find(int id);
void    room_post(room_t *r, event_t *e);
void    room_list_line(char *out, size_t n);

#endif
