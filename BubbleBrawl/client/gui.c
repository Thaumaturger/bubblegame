/*
 * gui.c — 圖形版 Client（SDL2 + SDL2_ttf）
 *
 * 與終端機版共用 core.c（連線、協定、本地狀態、指令邏輯），這裡只負責：
 *   1. 讀取鍵盤 / 滑鼠輸入（SDL 事件）
 *   2. 畫出圖形畫面（全部用基本圖形繪製，不需要圖片素材）
 *
 * 主迴圈仍是單執行緒：以 select() 等待 socket，最多等到下一個畫面時間點（約 60 FPS），
 * 時間到就處理 SDL 事件並重繪。
 *
 * 用法：./bubble_gui [host] [port]
 * 字型：自動尋找系統中文字型，也可用環境變數 BUBBLE_FONT 指定 .ttf / .ttc 路徑
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <SDL.h>
#include <SDL_ttf.h>
#include "client.h"

/* ---------------------------------------------------------------- 版面 */

#define WIN_W    1000
#define WIN_H    780
#define TILE     40
#define MAP_X    20
#define MAP_Y    64
#define MAP_PW   (MAP_W * TILE)             /* 600 */
#define MAP_PH   (MAP_H * TILE)             /* 520 */
#define PANEL_X  (MAP_X + MAP_PW + 20)      /* 640 */
#define PANEL_W  (WIN_W - PANEL_X - 20)     /* 340 */
#define LOG_Y    598
#define LOG_H    120
#define INPUT_Y  728
#define INPUT_H  40
#define FRAME_MS 16

enum { F_SMALL, F_NORMAL, F_BIG, F_TITLE, F_N };
static const int FONT_SIZE[F_N] = {15, 18, 26, 44};
enum { AL_L, AL_C, AL_R };

static const SDL_Color C_BG     = {22, 26, 38, 255};
static const SDL_Color C_BAR    = {32, 37, 54, 255};
static const SDL_Color C_PANEL  = {35, 41, 58, 255};
static const SDL_Color C_CARD   = {46, 54, 75, 255};
static const SDL_Color C_TEXT   = {232, 236, 244, 255};
static const SDL_Color C_DIM    = {138, 147, 168, 255};
static const SDL_Color C_ACCENT = {255, 200, 70, 255};
static const SDL_Color C_GOOD   = {110, 220, 120, 255};
static const SDL_Color C_BAD    = {255, 110, 110, 255};
static const SDL_Color C_BLUE   = {120, 180, 255, 255};
static const SDL_Color C_BTN    = {62, 120, 230, 255};
static const SDL_Color C_BTN2   = {84, 94, 120, 255};
static const SDL_Color C_BTN_OK = {56, 165, 90, 255};
static const SDL_Color C_BTN_NO = {200, 80, 80, 255};
static const SDL_Color SEAT_COL[MAX_PLAYERS] = {
    {235, 75, 75, 255}, {70, 130, 240, 255}, {60, 185, 95, 255}, {180, 95, 225, 255}};

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static TTF_Font     *g_font[F_N];
static SDL_Cursor   *g_cur_arrow, *g_cur_hand;

static int  g_mx, g_my, g_click, g_hover;   /* 滑鼠狀態（立即模式按鈕用） */
static int  g_user_quit;                    /* 使用者自己關閉視窗 / Ctrl+D */
static int  g_last_dir;                     /* 最後按下的方向（按住不放時持續移動） */
static long long g_swallow_ms;              /* 按 T 進入聊天時，吞掉同一下按鍵產生的文字 */
static char g_compose[64];                  /* 輸入法組字中的文字 */
static unsigned g_frame;

/* 玩家平滑移動動畫 */
static float g_rx[MAX_PLAYERS], g_ry[MAX_PLAYERS];
static int   g_fdx[MAX_PLAYERS], g_fdy[MAX_PLAYERS];
static int   g_anim_init;
static long long g_ended_at, g_last_list;
static cstate_t g_prev_st;

/* core.c 會呼叫（終端機版用來清畫面），圖形版每幀全部重畫，不需要 */
void ui_clear(void) {}

/* ---------------------------------------------------------- 繪圖工具 */

static SDL_Color rgba(int r, int g, int b, int a) { return (SDL_Color){(Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a}; }

static SDL_Color shade(SDL_Color c, float f)
{
    if (f < 1) return rgba((int)(c.r * f), (int)(c.g * f), (int)(c.b * f), c.a);
    f -= 1;
    return rgba(c.r + (int)((255 - c.r) * f), c.g + (int)((255 - c.g) * f), c.b + (int)((255 - c.b) * f), c.a);
}

static void set_col(SDL_Color c) { SDL_SetRenderDrawColor(g_ren, c.r, c.g, c.b, c.a); }

static void fill_rect(int x, int y, int w, int h, SDL_Color c)
{
    SDL_Rect r = {x, y, w, h};
    set_col(c);
    SDL_RenderFillRect(g_ren, &r);
}

static void hline(int x1, int x2, int y) { SDL_RenderDrawLine(g_ren, x1, y, x2, y); }

/* 以水平掃描線繪製，各列互不重疊，半透明顏色也不會有疊色 */
static void fill_ellipse(float cx, float cy, float rx, float ry, SDL_Color c)
{
    if (rx < 0.5f || ry < 0.5f) return;
    set_col(c);
    for (int dy = (int)-ry; dy <= (int)ry; dy++) {
        float t = 1.0f - (dy * dy) / (ry * ry);
        if (t < 0) continue;
        int dx = (int)(rx * sqrtf(t));
        hline((int)cx - dx, (int)cx + dx, (int)cy + dy);
    }
}

static void fill_circle(float cx, float cy, float r, SDL_Color c) { fill_ellipse(cx, cy, r, r, c); }

static void fill_ring(float cx, float cy, float ro, float ri, SDL_Color c)
{
    set_col(c);
    for (int dy = (int)-ro; dy <= (int)ro; dy++) {
        int xo = (int)sqrtf(ro * ro - dy * dy);
        if (abs(dy) < (int)ri) {
            int xi = (int)sqrtf(ri * ri - dy * dy);
            hline((int)cx - xo, (int)cx - xi, (int)cy + dy);
            hline((int)cx + xi, (int)cx + xo, (int)cy + dy);
        } else {
            hline((int)cx - xo, (int)cx + xo, (int)cy + dy);
        }
    }
}

static void fill_round(int x, int y, int w, int h, int r, SDL_Color c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    set_col(c);
    for (int dy = 0; dy < h; dy++) {
        int inset = 0;
        float d = -1;
        if (dy < r) d = r - dy - 0.5f;
        else if (dy >= h - r) d = dy - (h - r) + 0.5f;
        if (d >= 0) inset = r - (int)sqrtf((float)r * r - d * d);
        hline(x + inset, x + w - 1 - inset, y + dy);
    }
}

static void fill_tri(float x1, float y1, float x2, float y2, float x3, float y3, SDL_Color c)
{
    SDL_Vertex v[3] = {{{x1, y1}, c, {0, 0}}, {{x2, y2}, c, {0, 0}}, {{x3, y3}, c, {0, 0}}};
    SDL_RenderGeometry(g_ren, NULL, v, 3, NULL, 0);
}

/* ---------------------------------------------------------- 文字快取 */

/* 每次把文字轉成貼圖很耗時，所以把最近用過的字串貼圖快取起來 */
#define TC_N 512
typedef struct {
    SDL_Texture *t;
    uint32_t     hash;
    int          font, w, h;
    SDL_Color    c;
    unsigned     used;
    char         s[256];
} tcache_t;
static tcache_t g_tc[TC_N];

static uint32_t text_hash(const char *s, int font, SDL_Color c)
{
    uint32_t h = 2166136261u ^ (uint32_t)font ^ ((uint32_t)c.r << 8) ^ ((uint32_t)c.g << 16) ^ ((uint32_t)c.b << 24) ^ c.a;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static tcache_t *text_tex(int font, SDL_Color c, const char *s)
{
    if (!s || !*s || strlen(s) >= sizeof g_tc[0].s) return NULL;
    uint32_t h = text_hash(s, font, c);
    tcache_t *slot = NULL;
    for (int i = 0; i < TC_N; i++) {
        tcache_t *e = &g_tc[i];
        if (e->t) {
            if (e->hash == h && e->font == font && !memcmp(&e->c, &c, sizeof c) && !strcmp(e->s, s)) {
                e->used = g_frame;
                return e;
            }
            if (!slot || (slot->t && e->used < slot->used)) slot = e;
        } else if (!slot || slot->t) {
            slot = e;
        }
    }
    SDL_Surface *sf = TTF_RenderUTF8_Blended(g_font[font], s, c);
    if (!sf) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(g_ren, sf);
    int w = sf->w, hh = sf->h;
    SDL_FreeSurface(sf);
    if (!t) return NULL;
    if (slot->t) SDL_DestroyTexture(slot->t);
    slot->t = t;
    slot->hash = h;
    slot->font = font;
    slot->c = c;
    slot->w = w;
    slot->h = hh;
    slot->used = g_frame;
    snprintf(slot->s, sizeof slot->s, "%s", s);
    return slot;
}

static int font_h(int font) { return TTF_FontHeight(g_font[font]); }

static int text_at(int font, int x, int y, SDL_Color c, int align, const char *s)
{
    tcache_t *e = text_tex(font, c, s);
    if (!e) return 0;
    if (align == AL_C) x -= e->w / 2;
    else if (align == AL_R) x -= e->w;
    SDL_Rect d = {x, y, e->w, e->h};
    SDL_RenderCopy(g_ren, e->t, NULL, &d);
    return e->w;
}

static int textf(int font, int x, int y, SDL_Color c, int align, const char *fmt, ...) __attribute__((format(printf, 6, 7)));
static int textf(int font, int x, int y, SDL_Color c, int align, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return text_at(font, x, y, c, align, buf);
}

/* 訊息紀錄裡帶有終端機用的 ANSI 顏色碼（\033[91m…），轉成對應顏色分段繪製 */
static SDL_Color ansi_color(int code, SDL_Color cur)
{
    switch (code) {
    case 0:           return C_TEXT;
    case 31: case 91: return C_BAD;
    case 32: case 92: return C_GOOD;
    case 33: case 93: return C_ACCENT;
    case 34: case 94: return C_BLUE;
    case 35: case 95: return rgba(220, 140, 255, 255);
    case 36: case 96: return rgba(110, 220, 230, 255);
    case 37: case 97: return rgba(245, 245, 245, 255);
    case 90:          return C_DIM;
    }
    return cur;   /* 粗體、背景色等忽略 */
}

static void ansi_text(int font, int x, int y, const char *s)
{
    SDL_Color cur = C_TEXT;
    char seg[256];
    int n = 0;
    while (*s) {
        if (s[0] == '\033' && s[1] == '[') {
            if (n) { seg[n] = '\0'; x += text_at(font, x, y, cur, AL_L, seg); n = 0; }
            s += 2;
            int code = 0;
            while (*s && !isalpha((unsigned char)*s)) {
                if (isdigit((unsigned char)*s)) code = code * 10 + (*s - '0');
                else if (*s == ';') { cur = ansi_color(code, cur); code = 0; }
                s++;
            }
            cur = ansi_color(code, cur);
            if (*s) s++;
            continue;
        }
        if (n < (int)sizeof seg - 1) seg[n++] = *s;
        s++;
    }
    if (n) { seg[n] = '\0'; text_at(font, x, y, cur, AL_L, seg); }
}

/* ------------------------------------------------------------ 按鈕 */

/* 立即模式按鈕：每幀畫出來，滑鼠在上面放開就回傳 1 */
static int button(int x, int y, int w, int h, const char *label, int enabled, SDL_Color base)
{
    int hover = enabled && g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
    SDL_Color c = enabled ? base : rgba(64, 70, 88, 255);
    if (hover) { c = shade(c, 1.18f); g_hover = 1; }
    fill_round(x, y + 3, w, h, 9, shade(c, 0.6f));
    fill_round(x, y, w, h, 9, c);
    text_at(F_NORMAL, x + w / 2, y + (h - font_h(F_NORMAL)) / 2, enabled ? C_TEXT : C_DIM, AL_C, label);
    if (hover && g_click) { g_click = 0; return 1; }
    return 0;
}

/* ---------------------------------------------------------- 地圖元素 */

static void draw_floor(int x, int y, int s, int alt)
{
    fill_rect(x, y, s, s, alt ? rgba(198, 219, 178, 255) : rgba(208, 228, 189, 255));
}

static void draw_wall(int x, int y, int s)
{
    fill_rect(x, y, s, s, rgba(86, 96, 118, 255));
    fill_rect(x, y, s, s / 8, rgba(122, 134, 158, 255));
    fill_rect(x, y + s - s / 8, s, s / 8, rgba(60, 67, 84, 255));
    fill_rect(x + s / 8, y + s / 4, s - s / 4, s / 2, rgba(98, 109, 133, 255));
}

static void draw_block(int x, int y, int s)
{
    int m = s / 16 + 1;
    fill_rect(x + m, y + m, s - 2 * m, s - 2 * m, rgba(150, 98, 52, 255));
    fill_rect(x + 2 * m, y + 2 * m, s - 4 * m, s - 4 * m, rgba(206, 146, 82, 255));
    set_col(rgba(160, 108, 58, 255));
    for (int k = 1; k <= 2; k++) hline(x + 2 * m, x + s - 2 * m - 1, y + s * k / 3);
    SDL_RenderDrawLine(g_ren, x + 2 * m, y + 2 * m, x + s - 2 * m - 1, y + s - 2 * m - 1);
}

static void item_style(char it, SDL_Color *c, const char **label)
{
    switch (it) {
    case ITEM_BOMB:  *c = rgba(60, 170, 95, 255);  *label = "B+"; break;
    case ITEM_RANGE: *c = rgba(235, 125, 40, 255); *label = "R+"; break;
    default:         *c = rgba(170, 85, 220, 255); *label = "S+"; break;
    }
}

static void draw_item(int x, int y, int s, char it, long long now)
{
    SDL_Color c;
    const char *label;
    item_style(it, &c, &label);
    int bob = (int)(sinf((float)(now % 100000) / 260.0f + x * 0.3f) * s / 20);
    int m = s / 6;
    fill_round(x + m, y + m + 3 + bob, s - 2 * m, s - 2 * m, s / 6, shade(c, 0.6f));
    fill_round(x + m, y + m + bob, s - 2 * m, s - 2 * m, s / 6, c);
    text_at(F_SMALL, x + s / 2, y + (s - font_h(F_SMALL)) / 2 + bob, rgba(255, 255, 255, 255), AL_C, label);
}

static void draw_bomb(int x, int y, int s, long long left_ms, long long now, int show_num)
{
    float cx = x + s / 2.0f, cy = y + s / 2.0f + s / 20.0f;
    float speed = left_ms < 1000 ? 60.0f : 140.0f;
    float r = s * 0.32f + sinf((float)(now % 100000) / speed) * s / 30.0f;
    SDL_Color body = (left_ms < 1000 && (now / 100) % 2) ? rgba(150, 40, 40, 255) : rgba(40, 40, 56, 255);
    fill_ellipse(cx, cy + r * 0.85f, r * 0.9f, r * 0.3f, rgba(0, 0, 0, 70));
    fill_circle(cx, cy, r, body);
    fill_circle(cx - r * 0.35f, cy - r * 0.35f, r * 0.25f, rgba(255, 255, 255, 70));
    /* 引信與火花 */
    set_col(rgba(120, 85, 50, 255));
    SDL_RenderDrawLine(g_ren, (int)(cx + r * 0.45f), (int)(cy - r * 0.8f), (int)(cx + r * 0.75f), (int)(cy - r * 1.15f));
    fill_circle(cx + r * 0.8f, cy - r * 1.2f, s / 14.0f + ((now / 80) % 2), (now / 80) % 2 ? C_ACCENT : rgba(255, 110, 40, 255));
    if (show_num) {
        int sec = (int)((left_ms + 999) / 1000);
        if (sec < 1) sec = 1;
        textf(F_SMALL, (int)cx, (int)cy - font_h(F_SMALL) / 2, rgba(255, 255, 255, 255), AL_C, "%d", sec);
    }
}

static void draw_flame(int x, int y, int s, long long left_ms)
{
    int a = (int)(255 * left_ms / (FLAME_TICKS * TICK_MS));
    if (a > 255) a = 255;
    if (a < 90) a = 90;
    fill_round(x + 1, y + 1, s - 2, s - 2, s / 5, rgba(255, 120, 30, a));
    fill_round(x + s / 5, y + s / 5, s - 2 * s / 5, s - 2 * s / 5, s / 6, rgba(255, 225, 90, a));
}

static void draw_player(float cx, float cy, float r, int seat, int me, int fdx, int fdy)
{
    SDL_Color body = SEAT_COL[seat];
    fill_ellipse(cx, cy + r * 0.9f, r * 0.85f, r * 0.3f, rgba(0, 0, 0, 70));
    fill_circle(cx, cy, r, shade(body, 0.6f));
    fill_circle(cx, cy - 1, r - 2, body);
    fill_circle(cx - r * 0.42f, cy - r * 0.48f, r * 0.2f, rgba(255, 255, 255, 110));
    float er = r * 0.26f, pr = r * 0.13f;
    for (int side = -1; side <= 1; side += 2) {
        float ex = cx + side * r * 0.34f, ey = cy - r * 0.08f;
        fill_circle(ex, ey, er, rgba(255, 255, 255, 255));
        fill_circle(ex + fdx * pr * 0.8f, ey + fdy * pr * 0.8f, pr, rgba(25, 25, 35, 255));
    }
    if (me) fill_tri(cx - r * 0.4f, cy - r - 12, cx + r * 0.4f, cy - r - 12, cx, cy - r - 4, C_ACCENT);
}

static void draw_bubble(float cx, float cy, float r, long long left_ms)
{
    float br = r + 7 + sinf((float)(SDL_GetTicks() % 100000) / 150.0f) * 1.5f;
    fill_circle(cx, cy, br, rgba(150, 210, 255, 105));
    fill_ring(cx, cy, br, br - 2, rgba(225, 245, 255, 230));
    fill_circle(cx - br * 0.45f, cy - br * 0.5f, br * 0.17f, rgba(255, 255, 255, 210));
    if (left_ms > 0)
        textf(F_SMALL, (int)cx, (int)(cy - br - font_h(F_SMALL) + 2), rgba(255, 255, 255, 255), AL_C,
              "%.1f", left_ms / 1000.0);
}

/* 依玩家目前座標平滑移動（每格所需時間 = 移動冷卻） */
static void update_anim(long long dt)
{
    const game_t *g = &CL.g;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        const gplayer_t *p = &g->pl[i];
        float tx = (float)p->x, ty = (float)p->y;
        float dx = tx - g_rx[i], dy = ty - g_ry[i];
        if (!g_anim_init || fabsf(dx) + fabsf(dy) > 1.5f) {
            g_rx[i] = tx;
            g_ry[i] = ty;
            continue;
        }
        if (dx != 0 || dy != 0) {
            g_fdx[i] = (dx > 0) - (dx < 0);
            g_fdy[i] = (dy > 0) - (dy < 0);
        }
        float step = (float)dt / (float)(game_move_cd(p) * TICK_MS);
        if (fabsf(dx) <= step) g_rx[i] = tx; else g_rx[i] += dx > 0 ? step : -step;
        if (fabsf(dy) <= step) g_ry[i] = ty; else g_ry[i] += dy > 0 ? step : -step;
    }
    g_anim_init = 1;
}

static void draw_map(long long now)
{
    const game_t *g = &CL.g;
    fill_round(MAP_X - 6, MAP_Y - 6, MAP_PW + 12, MAP_PH + 12, 10, rgba(14, 17, 26, 255));
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++) {
            int px = MAP_X + x * TILE, py = MAP_Y + y * TILE;
            char c = g->map[y][x];
            if (c == CELL_WALL) { draw_wall(px, py, TILE); continue; }
            draw_floor(px, py, TILE, (x + y) % 2);
            if (c == CELL_BLOCK) draw_block(px, py, TILE);
            else if (game_is_item(c)) draw_item(px, py, TILE, c, now);
        }
    for (int b = 0; b < MAX_BOMB_TOTAL; b++)
        if (g->bombs[b].used)
            draw_bomb(MAP_X + g->bombs[b].x * TILE, MAP_Y + g->bombs[b].y * TILE, TILE,
                      CL.bomb_deadline[b] - now, now, 1);
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            if (CL.flame_until[y][x] > now)
                draw_flame(MAP_X + x * TILE, MAP_Y + y * TILE, TILE, CL.flame_until[y][x] - now);
    for (int pass = 0; pass < 2; pass++)   /* 先畫其他人，自己畫在最上層 */
        for (int i = 0; i < MAX_PLAYERS; i++) {
            const gplayer_t *p = &g->pl[i];
            if (p->status != PS_ALIVE && p->status != PS_TRAPPED) continue;
            if ((i == CL.my_seat) != (pass == 1)) continue;
            float cx = MAP_X + g_rx[i] * TILE + TILE / 2.0f, cy = MAP_Y + g_ry[i] * TILE + TILE / 2.0f;
            draw_player(cx, cy, TILE * 0.36f, i, i == CL.my_seat && p->status == PS_ALIVE, g_fdx[i], g_fdy[i]);
            if (p->status == PS_TRAPPED) draw_bubble(cx, cy, TILE * 0.36f, CL.trap_until[i] - now);
        }
}

/* ------------------------------------------------------------ 各畫面 */

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

static const char *team_name(int seat) { return seat % 2 == 0 ? "A 隊" : "B 隊"; }

static void draw_topbar(void)
{
    fill_rect(0, 0, WIN_W, 48, C_BAR);
    int w = text_at(F_BIG, 20, (48 - font_h(F_BIG)) / 2, C_ACCENT, AL_L, "泡泡大亂鬥");
    text_at(F_NORMAL, 30 + w, (48 - font_h(F_NORMAL)) / 2 + 2, C_DIM, AL_L, "BubbleBrawl");
    char info[160];
    if (CL.st >= CS_ROOM)
        snprintf(info, sizeof info, "%s　·　房間 #%d　·　P%d　·　%s", CL.nick, CL.room_id, CL.my_seat + 1, state_name(CL.st));
    else if (CL.nick[0] && CL.st != CS_NICK)
        snprintf(info, sizeof info, "%s　·　%s", CL.nick, state_name(CL.st));
    else
        snprintf(info, sizeof info, "%s", state_name(CL.st));
    text_at(F_NORMAL, WIN_W - 20, (48 - font_h(F_NORMAL)) / 2, C_TEXT, AL_R, info);
}

static void draw_welcome(long long now)
{
    int cx = WIN_W / 2;
    text_at(F_TITLE, cx, 110, C_ACCENT, AL_C, "泡泡大亂鬥");
    text_at(F_BIG, cx, 170, C_DIM, AL_C, "BubbleBrawl");
    /* 裝飾：一排地圖元素 */
    int s = 56, x0 = cx - s * 7 / 2, y0 = 240;
    for (int i = 0; i < 7; i++) draw_floor(x0 + i * s, y0, s, i % 2);
    draw_wall(x0, y0, s);
    draw_block(x0 + s, y0, s);
    draw_player(x0 + 2 * s + s / 2.0f, y0 + s / 2.0f, s * 0.36f, 0, 0, 1, 0);
    draw_bomb(x0 + 3 * s, y0, s, 3000, now, 0);
    draw_flame(x0 + 4 * s, y0, s, FLAME_TICKS * TICK_MS);
    draw_player(x0 + 5 * s + s / 2.0f, y0 + s / 2.0f, s * 0.36f, 1, 0, -1, 0);
    draw_bubble(x0 + 5 * s + s / 2.0f, y0 + s / 2.0f, s * 0.36f, 0);
    draw_item(x0 + 6 * s, y0, s, ITEM_RANGE, now);

    text_at(F_NORMAL, cx, 330, C_TEXT, AL_C, "放水球炸彈炸開方塊、撿道具，用火焰把對手困進泡泡！");
    text_at(F_NORMAL, cx, 362, C_TEXT, AL_C, "被困住 5 秒泡泡會破裂出局；被敵人碰到會立刻被戳破。");
    text_at(F_NORMAL, cx, 394, C_TEXT, AL_C, "團隊戰中，隊友碰到被困住的你可以把你救出來。");
    if (CL.st == CS_HELLO_SENT)
        text_at(F_BIG, cx, 470, C_ACCENT, AL_C, "登入中…");
    else
        text_at(F_BIG, cx, 470, C_ACCENT, AL_C, "↓ 在下方輸入暱稱，按 Enter 開始");
}

static void draw_lobby(void)
{
    int x = 20, y = 64, w = WIN_W - 40;
    text_at(F_BIG, x, y + 4, C_TEXT, AL_L, "房間列表");
    int bx = x + w;
    int busy = CL.st != CS_LOBBY;
    bx -= 120; if (button(bx, y, 120, 40, "重新整理", !busy, C_BTN2)) core_submit_line("/list");
    bx -= 150; if (button(bx, y, 140, 40, "建立團隊戰", !busy, C_BTN)) core_submit_line("/create team");
    bx -= 150; if (button(bx, y, 140, 40, "建立個人戰", !busy, C_BTN)) core_submit_line("/create");

    y += 64;
    fill_round(x, y, w, 500 - 64, 12, C_PANEL);
    if (CL.nrooms == 0) {
        text_at(F_NORMAL, WIN_W / 2, y + 150, C_DIM, AL_C, "目前沒有房間");
        text_at(F_NORMAL, WIN_W / 2, y + 185, C_DIM, AL_C, "按右上角「建立個人戰」或「建立團隊戰」開一個新房間");
    }
    int ry = y + 14;
    for (int i = 0; i < CL.nrooms; i++) {
        const roominfo_t *r = &CL.rooms[i];
        fill_round(x + 14, ry, w - 28, 46, 10, C_CARD);
        int ty = ry + (46 - font_h(F_NORMAL)) / 2;
        textf(F_BIG, x + 34, ry + (46 - font_h(F_BIG)) / 2, C_ACCENT, AL_L, "#%d", r->id);
        text_at(F_NORMAL, x + 110, ty, C_TEXT, AL_L, r->mode == MODE_TEAM ? "團隊戰" : "個人戰");
        /* 人數：小圓點 */
        for (int k = 0; k < MAX_PLAYERS; k++)
            fill_circle(x + 240 + k * 22, ry + 23, 7, k < r->count ? SEAT_COL[k] : rgba(70, 78, 98, 255));
        textf(F_NORMAL, x + 340, ty, C_DIM, AL_L, "%d / %d 人", r->count, MAX_PLAYERS);
        const char *st = "?";
        SDL_Color sc = C_DIM;
        switch (r->state) {
        case RSTATE_WAITING:   st = "等待中"; sc = C_GOOD; break;
        case RSTATE_COUNTDOWN: st = "倒數中"; sc = C_ACCENT; break;
        case RSTATE_PLAYING:   st = "遊戲中"; sc = C_BAD; break;
        case RSTATE_ENDED:     st = "結算中"; sc = C_BAD; break;
        }
        text_at(F_NORMAL, x + 480, ty, sc, AL_L, st);
        int can = !busy && r->state == RSTATE_WAITING && r->count < MAX_PLAYERS;
        char cmd[32];
        snprintf(cmd, sizeof cmd, "/join %d", r->id);
        if (button(x + w - 28 - 110, ry + 5, 100, 36, can ? "加入" : "無法加入", can, C_BTN_OK)) core_submit_line(cmd);
        ry += 54;
    }
    text_at(F_SMALL, x, 64 + 500 + 8, C_DIM, AL_L, "也可以在下方輸入指令：/create　/create team　/join 編號　/list　/quit");
}

static void draw_room(void)
{
    int x = 20, y = 64, w = WIN_W - 40;
    textf(F_BIG, x, y + 4, C_TEXT, AL_L, "房間 #%d　%s", CL.room_id, CL.mode == MODE_TEAM ? "團隊戰" : "個人戰");
    int ready = CL.roster[CL.my_seat].ready;
    if (button(x + w - 130, y, 130, 40, "離開房間", 1, C_BTN2)) core_submit_line("/leave");
    if (button(x + w - 280, y, 140, 40, ready ? "取消準備" : "準備！", 1, ready ? C_BTN_NO : C_BTN_OK))
        core_submit_line(ready ? "/unready" : "/ready");

    int cw = (w - 3 * 16) / 4, ch = 250, cy = y + 70;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        int cx = x + i * (cw + 16);
        int used = CL.roster[i].used;
        fill_round(cx, cy, cw, ch, 14, used ? C_CARD : C_PANEL);
        if (used) fill_round(cx, cy, cw, 8, 4, SEAT_COL[i]);
        if (i == CL.my_seat && used) text_at(F_NORMAL, cx + cw - 16, cy + 18, C_ACCENT, AL_R, "← 你");
        textf(F_NORMAL, cx + 16, cy + 18, used ? SEAT_COL[i] : C_DIM, AL_L, "P%d", i + 1);
        if (!used) {
            text_at(F_NORMAL, cx + cw / 2, cy + ch / 2 - 10, C_DIM, AL_C, "（空位）");
            continue;
        }
        draw_player(cx + cw / 2.0f, cy + 100, 38, i, 0, 0, 1);
        text_at(F_NORMAL, cx + cw / 2, cy + 152, C_TEXT, AL_C, CL.roster[i].nick);
        if (CL.mode == MODE_TEAM)
            text_at(F_SMALL, cx + cw / 2, cy + 180, i % 2 ? C_BLUE : C_BAD, AL_C, team_name(i));
        int rr = CL.roster[i].ready;
        fill_round(cx + cw / 2 - 50, cy + ch - 46, 100, 30, 15, rr ? rgba(40, 120, 60, 255) : rgba(70, 76, 96, 255));
        text_at(F_SMALL, cx + cw / 2, cy + ch - 46 + (30 - font_h(F_SMALL)) / 2, rr ? C_GOOD : C_DIM, AL_C,
                rr ? "已準備" : "未準備");
    }

    int sy = cy + ch + 30;
    if (CL.countdown > 0) {
        textf(F_TITLE, WIN_W / 2, sy, C_ACCENT, AL_C, "%d 秒後開始！", CL.countdown);
    } else {
        text_at(F_NORMAL, WIN_W / 2, sy, C_TEXT, AL_C, "所有人按「準備！」且至少 2 人就會開始");
        if (CL.mode == MODE_TEAM)
            text_at(F_SMALL, WIN_W / 2, sy + 30, C_DIM, AL_C, "團隊戰：P1、P3 為 A 隊（左側出生），P2、P4 為 B 隊（右側出生），兩隊都要有人");
        text_at(F_SMALL, WIN_W / 2, sy + 58, C_DIM, AL_C, "在下方輸入文字按 Enter 即可聊天");
    }
}

static void draw_panel(long long now)
{
    const game_t *g = &CL.g;
    int x = PANEL_X, y = MAP_Y - 6, w = PANEL_W;
    fill_round(x, y, w, MAP_PH + 12, 12, C_PANEL);
    x += 18;
    y += 14;

    long long left = CL.game_end_ms - now;
    if (left < 0) left = 0;
    int s = (int)(left / 1000);
    text_at(F_SMALL, x, y, C_DIM, AL_L, "剩餘時間");
    text_at(F_SMALL, x + w - 36, y, C_DIM, AL_R, CL.mode == MODE_TEAM ? "團隊戰" : "個人戰");
    textf(F_TITLE, x, y + 18, s < 30 ? C_BAD : C_TEXT, AL_L, "%d:%02d", s / 60, s % 60);
    y += 84;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        const gplayer_t *p = &g->pl[i];
        if (p->status == PS_EMPTY) continue;
        int dead = p->status == PS_DEAD;
        fill_round(x - 6, y, w - 24, 44, 10, i == CL.my_seat ? rgba(58, 66, 92, 255) : C_CARD);
        if (!dead) draw_player(x + 16, y + 22, 13, i, 0, 0, 1);
        else fill_circle(x + 16, y + 22, 13, rgba(80, 86, 104, 255));
        int ty = y + (44 - font_h(F_NORMAL)) / 2;
        int nw = text_at(F_NORMAL, x + 40, ty, dead ? C_DIM : C_TEXT, AL_L, seat_name(&CL, i));
        if (i == CL.my_seat) text_at(F_SMALL, x + 46 + nw, ty + 2, C_ACCENT, AL_L, "（你）");
        if (CL.mode == MODE_TEAM)
            text_at(F_SMALL, x + w - 110, ty + 2, i % 2 ? C_BLUE : C_BAD, AL_R, team_name(i));
        const char *st = p->status == PS_ALIVE ? "存活" : p->status == PS_TRAPPED ? "困住！" : "出局";
        SDL_Color sc = p->status == PS_ALIVE ? C_GOOD : p->status == PS_TRAPPED ? C_BLUE : C_DIM;
        text_at(F_NORMAL, x + w - 40, ty, sc, AL_R, st);
        y += 50;
    }

    y += 8;
    const gplayer_t *me = &g->pl[CL.my_seat];
    text_at(F_SMALL, x, y, C_DIM, AL_L, "你的能力");
    y += 26;
    draw_bomb(x - 4, y - 4, 32, 3000, now, 0);
    textf(F_NORMAL, x + 32, y, C_TEXT, AL_L, "%d/%d", me->bombs_max - game_active_bombs(g, CL.my_seat), me->bombs_max);
    draw_flame(x + 100, y - 2, 28, FLAME_TICKS * TICK_MS);
    textf(F_NORMAL, x + 134, y, C_TEXT, AL_L, "%d", me->range);
    draw_item(x + 176, y - 4, 32, ITEM_SPEED, 0);
    textf(F_NORMAL, x + 212, y, C_TEXT, AL_L, "%d", me->speed);
    y += 40;

    text_at(F_SMALL, x, y, C_DIM, AL_L, "道具");
    y += 24;
    const char *names[3] = {"炸彈+1", "火力+1", "速度+1"};
    const char its[3] = {ITEM_BOMB, ITEM_RANGE, ITEM_SPEED};
    for (int k = 0; k < 3; k++) {
        draw_item(x + k * 100 - 4, y - 4, 30, its[k], 0);
        text_at(F_SMALL, x + k * 100 + 28, y + 2, C_TEXT, AL_L, names[k]);
    }
    y += 40;

    const char *msg = NULL;
    SDL_Color mc = C_ACCENT;
    if (CL.st == CS_GAME && me->status == PS_TRAPPED) { msg = "你被困住了！"; mc = C_BLUE; }
    else if (CL.st == CS_GAME && me->status == PS_DEAD) { msg = "你已出局，觀戰中…"; mc = C_DIM; }
    else if (!CL.synced || CL.resync_pending) msg = "同步中…";
    if (msg) text_at(F_BIG, x, y, mc, AL_L, msg);

    int hy = MAP_Y + MAP_PH - 52;
    text_at(F_SMALL, x, hy, C_DIM, AL_L, "WASD / 方向鍵 移動　空白鍵 放炸彈");
    text_at(F_SMALL, x, hy + 22, C_DIM, AL_L, "T / Enter 聊天　X 認輸　Ctrl+D 離開");
}

static void draw_game(long long now)
{
    if (!CL.synced) {
        text_at(F_BIG, MAP_X + MAP_PW / 2, MAP_Y + MAP_PH / 2 - 20, C_DIM, AL_C, "載入地圖中…");
        return;
    }
    draw_map(now);
    draw_panel(now);

    if (CL.st == CS_ENDED) {
        fill_rect(MAP_X, MAP_Y, MAP_PW, MAP_PH, rgba(10, 12, 20, 170));
        text_at(F_TITLE, MAP_X + MAP_PW / 2, MAP_Y + MAP_PH / 2 - 60, C_ACCENT, AL_C, CL.result);
        long long back = ENDED_TICKS * TICK_MS - (now - g_ended_at);
        if (back < 0) back = 0;
        textf(F_NORMAL, MAP_X + MAP_PW / 2, MAP_Y + MAP_PH / 2 + 10, C_TEXT, AL_C, "%lld 秒後回到房間", (back + 999) / 1000);
    }
    if (CL.confirm) {
        fill_round(MAP_X + 100, MAP_Y + MAP_PH / 2 - 60, MAP_PW - 200, 120, 14, rgba(20, 24, 36, 235));
        text_at(F_BIG, MAP_X + MAP_PW / 2, MAP_Y + MAP_PH / 2 - 40, C_ACCENT, AL_C, "確定要認輸嗎？");
        text_at(F_NORMAL, MAP_X + MAP_PW / 2, MAP_Y + MAP_PH / 2 + 10, C_TEXT, AL_C, "按 Y 確認，其他鍵取消");
    }
}

static void draw_log(void)
{
    fill_round(20, LOG_Y, WIN_W - 40, LOG_H, 10, C_PANEL);
    int lh = (LOG_H - 12) / LOG_N;
    for (int i = 0; i < CL.log_n; i++) ansi_text(F_SMALL, 34, LOG_Y + 6 + i * lh, CL.log[i]);
}

static void draw_input(long long now)
{
    const char *prompt = NULL, *hint = "";
    switch (CL.st) {
    case CS_NICK:  prompt = "暱稱"; hint = "輸入暱稱（不可含空白或冒號），按 Enter"; break;
    case CS_LOBBY: prompt = "大廳"; hint = "可以直接按上方按鈕，或輸入 /help 查看指令"; break;
    case CS_ROOM:  prompt = "聊天"; hint = "輸入文字按 Enter 聊天；/ready 準備、/leave 離開"; break;
    case CS_ENDED: prompt = "聊天"; hint = "輸入文字按 Enter 聊天"; break;
    case CS_GAME:  if (CL.chat_mode) { prompt = "聊天"; hint = "輸入文字按 Enter 送出，Esc 取消"; } break;
    default: break;
    }
    int active = prompt != NULL;
    fill_round(20, INPUT_Y, WIN_W - 40, INPUT_H, 10, active ? C_ACCENT : C_PANEL);
    fill_round(22, INPUT_Y + 2, WIN_W - 44, INPUT_H - 4, 9, active ? rgba(30, 35, 52, 255) : C_PANEL);
    int ty = INPUT_Y + (INPUT_H - font_h(F_NORMAL)) / 2;
    if (!active) {
        const char *t = CL.st == CS_GAME ? "按 T 或 Enter 開始聊天" : "請稍候…";
        text_at(F_NORMAL, 38, ty, C_DIM, AL_L, t);
        return;
    }
    int x = 38;
    x += text_at(F_NORMAL, x, ty, C_ACCENT, AL_L, prompt);
    x += text_at(F_NORMAL, x, ty, C_ACCENT, AL_L, " › ");
    if (CL.input_len == 0 && !g_compose[0]) {
        if ((now / 530) % 2 == 0) fill_rect(x, ty + 2, 2, font_h(F_NORMAL) - 4, C_TEXT);
        text_at(F_NORMAL, x + 8, ty, C_DIM, AL_L, hint);
    } else {
        x += text_at(F_NORMAL, x, ty, C_TEXT, AL_L, CL.input);
        if (g_compose[0]) {
            int cw = text_at(F_NORMAL, x, ty, C_ACCENT, AL_L, g_compose);
            fill_rect(x, ty + font_h(F_NORMAL) - 2, cw, 2, C_ACCENT);
            x += cw;
        }
        if ((now / 530) % 2 == 0) fill_rect(x + 1, ty + 2, 2, font_h(F_NORMAL) - 4, C_TEXT);
    }
    SDL_Rect ime = {x, INPUT_Y, 10, INPUT_H};
    SDL_SetTextInputRect(&ime);
}

static void render(long long now)
{
    g_hover = 0;
    set_col(C_BG);
    SDL_RenderClear(g_ren);
    draw_topbar();
    switch (CL.st) {
    case CS_NICK:
    case CS_HELLO_SENT: draw_welcome(now); break;
    case CS_LOBBY:
    case CS_JOINING:    draw_lobby(); break;
    case CS_ROOM:       draw_room(); break;
    case CS_GAME:
    case CS_ENDED:      draw_game(now); break;
    }
    draw_log();
    draw_input(now);
    SDL_SetCursor(g_hover ? g_cur_hand : g_cur_arrow);
    SDL_RenderPresent(g_ren);
    g_click = 0;
    g_frame++;
}

/* ------------------------------------------------------------ 輸入 */

static int dir_of_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_w: case SDLK_UP:    return KEY_UP;
    case SDLK_s: case SDLK_DOWN:  return KEY_DOWN;
    case SDLK_a: case SDLK_LEFT:  return KEY_LEFT;
    case SDLK_d: case SDLK_RIGHT: return KEY_RIGHT;
    }
    return 0;
}

static int dir_held(const Uint8 *ks, int d)
{
    switch (d) {
    case KEY_UP:    return ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP];
    case KEY_DOWN:  return ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN];
    case KEY_LEFT:  return ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_LEFT];
    case KEY_RIGHT: return ks[SDL_SCANCODE_D] || ks[SDL_SCANCODE_RIGHT];
    }
    return 0;
}

/* 按住方向鍵：冷卻一結束就繼續走（不依賴系統的按鍵重複速度） */
static void poll_held_dirs(void)
{
    if (CL.st != CS_GAME || core_line_mode() || CL.confirm || !CL.synced) return;
    const gplayer_t *p = &CL.g.pl[CL.my_seat];
    if (now_ms() - CL.last_move_ms < (long long)game_move_cd(p) * TICK_MS - 20) return;
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    static const int dirs[4] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
    if (g_last_dir && dir_held(ks, g_last_dir)) { core_game_key(g_last_dir); return; }
    for (int i = 0; i < 4; i++)
        if (dir_held(ks, dirs[i])) { g_last_dir = dirs[i]; core_game_key(dirs[i]); return; }
}

static void on_keydown(const SDL_KeyboardEvent *k)
{
    SDL_Keycode sym = k->keysym.sym;
    if ((k->keysym.mod & KMOD_CTRL) && (sym == SDLK_d || sym == SDLK_c)) {
        g_user_quit = 1;
        core_quit("已離開遊戲");
        return;
    }
    if (CL.st == CS_HELLO_SENT || CL.st == CS_JOINING) return;

    if (core_line_mode()) {
        if (g_compose[0]) return;   /* 輸入法組字中，按鍵交給輸入法 */
        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) core_line_key('\r');
        else if (sym == SDLK_BACKSPACE) core_line_key(0x7f);
        else if (sym == SDLK_ESCAPE) core_cancel_chat();
        return;
    }
    if (CL.st != CS_GAME) return;

    int d = dir_of_key(sym);
    if (d && !CL.confirm) {
        if (k->repeat) return;     /* 按住由 poll_held_dirs 處理 */
        g_last_dir = d;
        core_game_key(d);
        return;
    }
    if (k->repeat) return;
    int key = 0;
    if (sym == SDLK_SPACE) key = ' ';
    else if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) key = '\r';
    else if (sym >= SDLK_a && sym <= SDLK_z) key = (int)sym;
    if (!key) return;
    core_game_key(key);
    if (CL.chat_mode) g_swallow_ms = now_ms();   /* 按 T 進入聊天：不要把 t 打進輸入框 */
}

static void on_text(const char *t)
{
    if (CL.st == CS_HELLO_SENT || CL.st == CS_JOINING || !core_line_mode()) return;
    if (g_swallow_ms && now_ms() - g_swallow_ms < 200 && (!strcmp(t, "t") || !strcmp(t, "T"))) {
        g_swallow_ms = 0;
        return;
    }
    g_swallow_ms = 0;
    g_compose[0] = '\0';
    for (const char *p = t; *p; p++) core_line_key((unsigned char)*p);
}

static void handle_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            g_user_quit = 1;
            core_quit("已關閉視窗");
            break;
        case SDL_KEYDOWN:
            on_keydown(&e.key);
            break;
        case SDL_TEXTINPUT:
            on_text(e.text.text);
            break;
        case SDL_TEXTEDITING:
            snprintf(g_compose, sizeof g_compose, "%s", e.edit.text);
            break;
        case SDL_MOUSEMOTION:
            g_mx = e.motion.x;
            g_my = e.motion.y;
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
                g_mx = e.button.x;
                g_my = e.button.y;
                g_click = 1;
            }
            break;
        }
    }
}

/* ------------------------------------------------------------ 初始化 */

static const char *find_font(void)
{
    static const char *paths[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/mnt/c/Windows/Fonts/msjh.ttc",      /* WSL：Windows 的微軟正黑體 */
        "/mnt/c/Windows/Fonts/msyh.ttc",
        "/mnt/c/Windows/Fonts/mingliu.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        NULL};
    const char *env = getenv("BUBBLE_FONT");
    if (env && access(env, R_OK) == 0) return env;
    for (int i = 0; paths[i]; i++)
        if (access(paths[i], R_OK) == 0) return paths[i];
    fprintf(stderr, "找不到中文字型，中文可能無法顯示。\n"
                    "可安裝：sudo apt install fonts-noto-cjk，或用 BUBBLE_FONT=字型路徑 指定\n");
    if (access("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", R_OK) == 0)
        return "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
    return NULL;
}

static int gui_init(void)
{
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL 初始化失敗：%s\n", SDL_GetError());
        return -1;
    }
    if (TTF_Init() < 0) {
        fprintf(stderr, "SDL_ttf 初始化失敗：%s\n", TTF_GetError());
        return -1;
    }
    const char *fp = find_font();
    if (!fp) { fprintf(stderr, "沒有可用的字型\n"); return -1; }
    for (int i = 0; i < F_N; i++) {
        g_font[i] = TTF_OpenFont(fp, FONT_SIZE[i]);
        if (!g_font[i]) { fprintf(stderr, "無法開啟字型 %s：%s\n", fp, TTF_GetError()); return -1; }
    }
    g_win = SDL_CreateWindow("泡泡大亂鬥 BubbleBrawl", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) { fprintf(stderr, "無法建立視窗：%s\n", SDL_GetError()); return -1; }
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) { fprintf(stderr, "無法建立繪圖器：%s\n", SDL_GetError()); return -1; }
    SDL_RenderSetLogicalSize(g_ren, WIN_W, WIN_H);   /* 視窗縮放時等比例放大 */
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);
    g_cur_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    g_cur_hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    SDL_StartTextInput();
    return 0;
}

static void gui_shutdown(void)
{
    for (int i = 0; i < TC_N; i++)
        if (g_tc[i].t) SDL_DestroyTexture(g_tc[i].t);
    for (int i = 0; i < F_N; i++)
        if (g_font[i]) TTF_CloseFont(g_font[i]);
    if (g_cur_arrow) SDL_FreeCursor(g_cur_arrow);
    if (g_cur_hand) SDL_FreeCursor(g_cur_hand);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    TTF_Quit();
    SDL_Quit();
}

/* 連線結束（不是使用者自己關的）：顯示原因，等使用者按鍵或關閉視窗 */
static void show_exit_screen(const char *reason)
{
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT || e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONUP) return;
        set_col(C_BG);
        SDL_RenderClear(g_ren);
        draw_topbar();
        text_at(F_TITLE, WIN_W / 2, 260, C_ACCENT, AL_C, "連線已結束");
        text_at(F_BIG, WIN_W / 2, 340, C_TEXT, AL_C, reason);
        text_at(F_NORMAL, WIN_W / 2, 420, C_DIM, AL_C, "按任意鍵或關閉視窗離開");
        SDL_RenderPresent(g_ren);
        g_frame++;
        SDL_Delay(30);
    }
}

/* ------------------------------------------------------------ 主程式 */

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "7777";

    signal(SIGPIPE, SIG_IGN);
    int sock = core_connect(host, port);
    if (sock < 0) { fprintf(stderr, "無法連線到 %s:%s（Server 有先啟動嗎？）\n", host, port); return 1; }
    if (gui_init() < 0) { close(sock); gui_shutdown(); return 1; }

    core_init(sock, host, port);
    long long last = now_ms(), next_frame = last;

    while (!core_quitting()) {
        /* 等 socket 資料，最多等到下一個畫面時間點 */
        long long wait = next_frame - now_ms();
        if (wait < 0) wait = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        struct timeval tv = {0, (suseconds_t)(wait * 1000)};
        int n = select(sock + 1, &rfds, NULL, NULL, &tv);
        if (n > 0 && FD_ISSET(sock, &rfds)) core_on_readable();
        if (core_quitting()) break;

        long long now = now_ms();
        if (now < next_frame) continue;
        next_frame = now + FRAME_MS;

        handle_events();
        poll_held_dirs();
        core_periodic();

        /* 狀態轉換時的畫面處理 */
        if (CL.st != g_prev_st) {
            if (CL.st == CS_GAME) g_anim_init = 0;
            if (CL.st == CS_ENDED) g_ended_at = now;
            if (CL.st == CS_LOBBY) g_last_list = now;
            g_compose[0] = '\0';
            g_prev_st = CL.st;
        }
        /* 大廳每 2 秒自動更新房間列表 */
        if (CL.st == CS_LOBBY && now - g_last_list > 2000) {
            core_submit_line("/list");
            g_last_list = now;
        }
        if (CL.st == CS_GAME || CL.st == CS_ENDED) update_anim(now - last);
        last = now;
        render(now);
        CL.dirty = 0;
    }

    core_finish();
    if (!g_user_quit) show_exit_screen(core_exit_reason());
    gui_shutdown();
    printf("%s\n", core_exit_reason());
    return 0;
}
