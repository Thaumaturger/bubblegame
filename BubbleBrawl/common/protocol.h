/*
 * protocol.h — Client 與 Server 共用的通訊協定定義
 *
 * 訊息格式：一行一則文字訊息，以 '\n' 結尾
 *     <CODE> <arg1> <arg2> ...\n
 * CODE 為訊息型態代碼；遊戲事件（540~560）的第一個參數固定為序號 seq。
 */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#define DEFAULT_PORT   7777
#define MAX_LINE       4096   /* 單行訊息最大長度 */
#define NICK_MAX       24     /* 暱稱最大位元組數（UTF-8） */
#define CHAT_MAX       200    /* 聊天訊息最大位元組數 */
#define MAX_ROOMS      8
#define MAX_CLIENTS    64
#define HEARTBEAT_SEC  5      /* Client 每 5 秒送一次 PING */
#define TIMEOUT_SEC    15     /* 超過 15 秒沒收到任何資料視為斷線 */

/* ---------- Client → Server ---------- */
enum {
    C_HELLO     = 100,  /* nickname          設定暱稱 */
    C_LIST      = 110,  /*                   查詢房間列表 */
    C_CREATE    = 111,  /* mode(0個人/1團隊) 建立房間 */
    C_JOIN      = 112,  /* room_id           加入房間 */
    C_LEAVE     = 113,  /*                   離開房間 */
    C_READY     = 120,  /* 1|0               準備 / 取消準備 */
    C_MOVE      = 200,  /* U|D|L|R           移動 */
    C_BOMB      = 201,  /*                   放水球炸彈 */
    C_CHAT      = 210,  /* text...           聊天 */
    C_SURRENDER = 290,  /*                   認輸 */
    C_RESYNC    = 298,  /*                   要求完整狀態同步 */
    C_QUIT      = 299,  /*                   離線 */
    C_PING      = 900   /*                   心跳 */
};

/* ---------- Server → Client ---------- */
enum {
    S_WELCOME   = 500,  /* player_id */
    S_ROOMLIST  = 510,  /* n id:mode:count:state ... */
    S_JOINED    = 511,  /* room_id seat mode   （告知你是第幾位玩家） */
    S_ROSTER    = 512,  /* mode n seat:nick:ready ... */
    S_LEFT      = 513,  /* seat reason */
    S_COUNTDOWN = 520,  /* seconds */
    S_CANCEL    = 521,  /*                    倒數取消 */
    S_START     = 522,  /*                    遊戲開始（隨後送 SNAPSHOT） */
    S_BACK      = 523,  /*                    結算結束，回到房間 */
    S_MOVED     = 540,  /* seq seat x y */
    S_BOMB      = 541,  /* seq x y owner timer range */
    S_EXPLODE   = 542,  /* seq x y n x,y x,y ... */
    S_CELL      = 543,  /* seq x y ch         地圖格子變化 */
    S_PICKUP    = 544,  /* seq seat x y item */
    S_TRAPPED   = 545,  /* seq seat */
    S_RESCUED   = 546,  /* seq seat by */
    S_DIED      = 547,  /* seq seat reason by */
    S_SNAPSHOT  = 550,  /* seq ticks_left mode map p0..p3 nb bombs... */
    S_HASH      = 551,  /* seq hash           狀態一致性檢查 */
    S_GAMEOVER  = 560,  /* seq kind id */
    S_CHAT      = 600,  /* seat text... */
    S_ERR       = 700,  /* err_code message */
    S_SHUTDOWN  = 799,  /*                    伺服器關閉 */
    S_PONG      = 901
};

/* ---------- 錯誤代碼（S_ERR 的參數） ---------- */
enum {
    E_FORMAT      = 1,  /* 訊息格式錯誤 */
    E_UNKNOWN     = 2,  /* 未知的訊息代碼 */
    E_STATE       = 3,  /* 目前狀態不允許此動作 */
    E_NO_ROOM     = 4,  /* 房間不存在 */
    E_FULL        = 5,  /* 房間已滿 / 沒有空房間 */
    E_BUSY        = 6,  /* 房間遊戲進行中 */
    E_NICK        = 7,  /* 暱稱不合法 */
    E_SERVER_FULL = 8,  /* 伺服器人數已滿 */
    E_ACTION      = 9   /* 不合法的遊戲動作 */
};

enum { LEFT_NORMAL = 0, LEFT_DISCONNECT = 1 };
enum { DIE_BUBBLE = 0, DIE_POPPED = 1, DIE_SURRENDER = 2, DIE_LEFT = 3 };
enum { GO_DRAW = 0, GO_PLAYER = 1, GO_TEAM = 2 };

/* 房間狀態（同時用於房間列表） */
enum { RSTATE_CLOSED = 0, RSTATE_WAITING, RSTATE_COUNTDOWN, RSTATE_PLAYING, RSTATE_ENDED };

#endif
