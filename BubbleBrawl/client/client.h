/*
 * client.h — Client 端狀態（stateful client）
 *
 * Client 保存一份本地遊戲狀態（地圖、玩家、炸彈），用來：
 *   1. 在送出前檢查動作是否合法（撞牆、炸彈數上限、被困住時不能動…）
 *   2. 即時繪製畫面（炸彈倒數、火焰動畫以本地時間推算）
 * 並透過「事件序號 + 狀態雜湊」與 Server 比對，不一致時要求 RESYNC。
 */
#ifndef CLIENT_H
#define CLIENT_H

#include "protocol.h"
#include "game.h"

#define LOG_N 6

typedef enum {
    CS_NICK,        /* 輸入暱稱 */
    CS_HELLO_SENT,  /* 等待 WELCOME */
    CS_LOBBY,       /* 大廳 */
    CS_JOINING,     /* 等待加入結果 */
    CS_ROOM,        /* 房間內等待 */
    CS_GAME,        /* 遊戲中 */
    CS_ENDED        /* 結算畫面 */
} cstate_t;

typedef struct { int id, mode, count, state; } roominfo_t;

typedef struct {
    cstate_t st;
    int      sock;
    char     nick[NICK_MAX + 1];
    int      my_id;

    roominfo_t rooms[MAX_ROOMS];
    int        nrooms;

    int  room_id, my_seat, mode, countdown;
    struct { int used; char nick[NICK_MAX + 1]; int ready; } roster[MAX_PLAYERS];

    /* 遊戲狀態 */
    game_t    g;
    int       seq, synced, resync_pending;
    long long bomb_deadline[MAX_BOMB_TOTAL];
    long long flame_until[MAP_H][MAP_W];
    long long trap_until[MAX_PLAYERS];  /* 泡泡破裂時間（本地推算，顯示倒數用） */
    long long game_end_ms;
    long long last_move_ms, last_warn_ms;
    int       queued_dir;               /* 冷卻中按下的方向 */
    char      result[160];

    /* 輸入與畫面 */
    char input[256];
    int  input_len;
    int  chat_mode, confirm;
    char log[LOG_N][240];
    int  log_n;
    int  dirty;

    long long last_recv_ms, last_ping_ms;
} cl_t;

/* 方向鍵代碼（傳給 core_game_key） */
#define KEY_UP    ('A' + 256)
#define KEY_DOWN  ('B' + 256)
#define KEY_RIGHT ('C' + 256)
#define KEY_LEFT  ('D' + 256)

extern cl_t CL;

long long now_ms(void);
void      cl_log(cl_t *cl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
const char *seat_name(const cl_t *cl, int seat);

/* ---- core.c：終端機版與圖形版共用的連線 / 協定 / 狀態邏輯 ---- */
int         core_connect(const char *host, const char *port);
void        core_init(int sock, const char *host, const char *port);
void        core_on_readable(void);              /* socket 可讀時呼叫 */
void        core_periodic(void);                 /* 每輪主迴圈呼叫：緩衝移動、心跳、逾時 */
void        core_submit_line(const char *line);  /* 送出一行文字指令或聊天 */
void        core_line_key(unsigned char c);      /* 文字輸入模式：逐位元組輸入（\r 送出、0x7f 刪除） */
void        core_game_key(int key);              /* 遊戲按鍵：WASD、KEY_*、空白、T、X… */
int         core_line_mode(void);                /* 目前是否為文字輸入模式 */
void        core_cancel_chat(void);
void        core_quit(const char *reason);
int         core_quitting(void);
const char *core_exit_reason(void);
void        core_finish(void);                   /* 送 QUIT 並關閉連線 */

/* ---- 畫面（終端機版在 ui.c，圖形版在 gui.c） ---- */
void ui_init(void);
void ui_restore(void);
void ui_clear(void);
void ui_render(cl_t *cl);

#endif
