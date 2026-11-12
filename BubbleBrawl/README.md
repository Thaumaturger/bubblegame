# 泡泡大亂鬥 BubbleBrawl

網路程式設計期末專題。玩法參考爆爆王（Crazy Arcade）的即時制多人連線遊戲，Server 與 Client 皆以 C 撰寫，使用 TCP、pthread 與 select，介面為終端機文字模式（ANSI escape sequence，不需額外函式庫）。

2～4 位玩家在 15×13 的地圖上放水球炸彈，炸開方塊、撿道具，用火焰把對手困在泡泡裡。被困住的玩家 5 秒後泡泡破裂出局；這段期間如果被敵人碰到會立刻被戳破，在團隊戰中被隊友碰到則會被救出。最後存活的玩家（或隊伍）獲勝，每局限時 3 分鐘。

## 編譯與執行

需要 Linux 或 WSL（使用了 `pthread_condattr_setclock`、`MSG_NOSIGNAL` 等 Linux 介面），以及 gcc 與 make。

```bash
make                         # 產生 bubble_server 與 bubble_client
./bubble_server [port]       # 預設 port 7777
./bubble_client [host] [port]  # 預設 127.0.0.1 7777，每位玩家開一個終端機
```

終端機請至少 80×24，並使用 UTF-8 編碼。

### 圖形版 Client（建議）

除了終端機版，也有用 SDL2 繪製的圖形版，可以用滑鼠點按鈕建房、加入、準備，輸入框一直看得到。兩種 Client 可以混在同一局一起玩。

```bash
sudo apt install -y libsdl2-dev libsdl2-ttf-dev   # 只需安裝一次
make gui                                          # 產生 bubble_gui
./bubble_gui [host] [port]                        # 預設 127.0.0.1 7777，會開出遊戲視窗
```

WSL 需要 Windows 10 21H2 以上或 Windows 11（內建 WSLg 才能開視窗）。中文字型會自動尋找（Noto CJK、文泉驛，或 WSL 下 Windows 的微軟正黑體）；找不到時可以執行 `sudo apt install fonts-noto-cjk`，或用 `BUBBLE_FONT=字型路徑 ./bubble_gui` 指定。

## 操作方式

大廳與房間使用文字指令：

| 位置 | 指令 |
|---|---|
| 大廳 | `/list` 更新房間列表、`/create` 建立個人戰、`/create team` 建立團隊戰、`/join 編號` 加入房間 |
| 房間 | `/ready` 準備、`/unready` 取消準備、`/leave` 離開房間、其他文字直接送出為聊天 |
| 任何時候 | `/quit` 或 Ctrl+D 離開遊戲 |

遊戲中按鍵：WASD 或方向鍵移動、空白鍵放炸彈、T 或 Enter 聊天（Esc 取消）、X 認輸（需按 y 確認）、Ctrl+D 離開。

道具：`B+` 炸彈數量 +1、`R+` 火力範圍 +1、`S+` 移動速度 +1。

## 檔案結構

```
common/protocol.h   訊息代碼、錯誤碼、常數（兩邊共用）
common/net.c/.h     send_all、以行切割的接收緩衝、暱稱檢查
common/game.c/.h    遊戲規則（移動、放炸彈、道具、狀態雜湊、快照編解碼），兩邊共用
server/main.c       accept 迴圈、client thread、大廳指令
server/room.c       room thread、狀態 × 事件對照表、遊戲模擬
client/core.c       Client 共用核心：連線、Server 訊息處理、本地狀態、指令與按鍵邏輯
client/main.c       終端機版：select 迴圈、讀取鍵盤（raw mode）
client/ui.c         終端機版畫面繪製（ANSI）
client/gui.c        圖形版：SDL2 事件與繪圖（make gui）
tests/              自動化測試（Python，只用來測試，不屬於專題程式本體）
```

## 系統架構

### Server：多執行緒

```
main thread ── accept() ──┬─> client thread #1 ─┐  recv → 切行 → 解析
                          ├─> client thread #2 ─┼─> 大廳指令直接處理
                          └─> client thread #N ─┘   房間指令轉成事件放入佇列
                                                          │
                                                          ▼
                                          room thread（每個房間一條）
                                          依「房間狀態 × 事件」查表處理
                                          每 50ms 推進一次遊戲（20 Hz）
```

遊戲狀態只由 room thread 修改，因此遊戲邏輯本身不需要加鎖。client thread 與 room thread 之間只透過「事件佇列」（mutex + condition variable）溝通。room thread 用 `pthread_cond_timedwait` 等待事件，等到下一個 tick 的時間點就推進遊戲，所以同一條執行緒同時處理玩家輸入與計時（炸彈倒數、火焰、泡泡計時、回合倒數）。

### Client：單執行緒 + select

Client 用 `select()` 同時監聽鍵盤（stdin）與 socket，逾時 50ms 用來重繪畫面與送心跳。鍵盤以 raw mode 讀取，所以按鍵不需按 Enter 就能即時反應。

圖形版同樣是單執行緒：主迴圈用 `select()` 等 socket，最多等到下一個畫面時間點（約 60 FPS），時間到就處理 SDL 的鍵盤 / 滑鼠事件並重繪。協定處理、本地狀態與指令邏輯都在 `client/core.c`，兩種 Client 共用，差別只在「怎麼讀輸入」和「怎麼畫畫面」。

### Server 權威 + Stateful Client

Server 是遊戲狀態的唯一權威，所有動作都由 Server 做最終判定。Client 保存一份完整的本地狀態（地圖、玩家位置與能力、炸彈），用途有三個：

1. **送出前先檢查**：撞牆、走進炸彈、炸彈數已達上限、被泡泡困住時移動、加入已滿或遊戲中的房間，都在 Client 端直接擋下，不會送到 Server。
2. **本地動畫**：炸彈倒數秒數與火焰持續時間由 Client 依本地時間推算，Server 不必每個 tick 都送資料。
3. **輸入緩衝**：移動冷卻中按下的方向會先記住，冷卻結束時才送出，快速連按不會掉鍵。

Client 與 Server 使用同一份 `common/game.c` 做規則判斷，兩邊的規則保證一致。

## 通訊協定

一行一則文字訊息，以 `\n` 結尾：`<代碼> <參數1> <參數2> ...`。採用文字協定，可以直接用 `nc localhost 7777` 手動測試 Server。因為 TCP 是位元串流，兩端都會把收到的資料累積在緩衝區，遇到 `\n` 才切出一則完整訊息。

### Client → Server

| 代碼 | 名稱 | 參數 | 說明 |
|---|---|---|---|
| 100 | HELLO | nickname | 設定暱稱（1~24 位元組，不可含空白與冒號） |
| 110 | LIST | — | 查詢房間列表 |
| 111 | CREATE | mode | 建立房間，0 個人戰 / 1 團隊戰 |
| 112 | JOIN | room_id | 加入房間 |
| 113 | LEAVE | — | 離開房間 |
| 120 | READY | 1 或 0 | 準備 / 取消準備 |
| 200 | MOVE | U/D/L/R | 移動 |
| 201 | BOMB | — | 放炸彈 |
| 210 | CHAT | text | 聊天 |
| 290 | SURRENDER | — | 認輸 |
| 298 | RESYNC | — | 要求完整狀態快照 |
| 299 | QUIT | — | 離線 |
| 900 | PING | — | 心跳 |

### Server → Client

| 代碼 | 名稱 | 參數 | 說明 |
|---|---|---|---|
| 500 | WELCOME | player_id | 登入成功 |
| 510 | ROOMLIST | n id:mode:count:state … | 房間列表 |
| 511 | JOINED | room seat mode | 加入成功，seat 0 表示第 1 位加入的玩家 |
| 512 | ROSTER | mode n seat:nick:ready … | 房間成員名單 |
| 513 | LEFT | seat reason | 有人離開（0）或斷線（1） |
| 520 | COUNTDOWN | seconds | 開始倒數 |
| 521 | CANCEL | — | 倒數取消 |
| 522 | START | — | 遊戲開始，隨後送 SNAPSHOT |
| 523 | BACK | — | 結算結束，回到房間 |
| 540 | MOVED | seq seat x y | 玩家移動 |
| 541 | BOMB | seq x y owner timer range | 放置炸彈 |
| 542 | EXPLODE | seq x y n x,y … | 爆炸與火焰範圍 |
| 543 | CELL | seq x y ch | 地圖格子改變（方塊炸毀、道具出現或燒毀） |
| 544 | PICKUP | seq seat x y item | 撿到道具 |
| 545 | TRAPPED | seq seat | 被泡泡困住 |
| 546 | RESCUED | seq seat by | 被隊友救出 |
| 547 | DIED | seq seat reason by | 出局（0 泡泡破裂、1 被戳破、2 認輸、3 離開） |
| 550 | SNAPSHOT | seq ticks mode map players bombs | 完整遊戲狀態 |
| 551 | HASH | seq hash | 狀態雜湊，每秒一次 |
| 560 | GAMEOVER | seq kind id | 結束（0 平手、1 個人獲勝、2 隊伍獲勝） |
| 600 | CHAT | seat text | 聊天廣播 |
| 700 | ERR | code message | 錯誤 |
| 799 | SHUTDOWN | — | 伺服器關閉 |
| 901 | PONG | — | 心跳回應 |

錯誤代碼：1 格式錯誤、2 未知代碼、3 目前狀態不允許、4 房間不存在、5 房間已滿、6 房間遊戲中、7 暱稱不合法、8 伺服器已滿、9 不合法的遊戲動作。

### 狀態一致性機制

1. 每個遊戲事件（540~560）都帶有遞增的序號 `seq`。Client 收到的序號不連續時，代表漏收或狀態錯亂，會送出 `298 RESYNC`。
2. Server 每秒送出一次 `551 HASH`，內容是當下遊戲狀態的 FNV-1a 雜湊（地圖、炸彈位置、所有玩家的狀態與能力）。Client 對自己的本地狀態算同一個雜湊，不同就送 RESYNC。
3. Server 收到 RESYNC 後回傳 `550 SNAPSHOT`，Client 直接以快照覆蓋本地狀態。

## 狀態機

### Server：連線（session）狀態

| 狀態 | 可接受的訊息 | 其他訊息 |
|---|---|---|
| CONNECTED | HELLO → LOBBY | ERR 3 |
| LOBBY | LIST、CREATE / JOIN → JOINING | ERR 3（已知代碼）或 ERR 2 |
| JOINING | 等 room thread 回覆後轉為 IN_ROOM 或回到 LOBBY | ERR 3 |
| IN_ROOM | LEAVE、READY、MOVE、BOMB、CHAT、SURRENDER、RESYNC 轉成事件送進房間 | ERR 3 或 ERR 2 |

PING 與 QUIT 在任何狀態都可以使用。

### Server：房間狀態 × 事件

`server/room.c` 中的 `TABLE[狀態][事件]` 就是這張表，每一格都有對應的處理函式，任何組合都不會卡住。

| 狀態＼事件 | JOIN | LEAVE | READY | MOVE | BOMB | CHAT | SURRENDER | RESYNC | 斷線 | 計時 |
|---|---|---|---|---|---|---|---|---|---|---|
| CLOSED | ERR 4 | 忽略 | 忽略 | 忽略 | 忽略 | 忽略 | 忽略 | 忽略 | 釋放連線 | 不計時 |
| WAITING | 分配座位，回 JOINED | 移除；沒人則 → CLOSED | 更新準備；全員準備且 ≥2 人 → COUNTDOWN | ERR 3 | ERR 3 | 廣播 | ERR 3 | ERR 3 | 同 LEAVE | 不計時 |
| COUNTDOWN | ERR 6 | 移除並取消倒數 → WAITING | 取消準備 → WAITING | ERR 3 | ERR 3 | 廣播 | ERR 3 | ERR 3 | 同 LEAVE | 3 秒後 → PLAYING |
| PLAYING | ERR 6 | 判定出局、移除、檢查勝負 | ERR 3 | 緩衝方向，冷卻後執行 | 合法則放置，否則 ERR 9 | 廣播 | 判定出局、檢查勝負 | 回傳 SNAPSHOT | 同 LEAVE | 推進遊戲；分出勝負或時間到 → ENDED |
| ENDED | ERR 6 | 移除 | ERR 3 | 忽略 | 忽略 | 廣播 | ERR 3 | 回傳 SNAPSHOT | 同 LEAVE | 5 秒後 → WAITING |

### Client 狀態

```
NICK → HELLO_SENT → LOBBY ⇄ JOINING → ROOM → GAME → ENDED → ROOM
```

在 ROOM 收到 START 進入 GAME；GAME 收到 GAMEOVER 進入 ENDED；ENDED 收到 BACK 回到 ROOM；收到自己的 LEFT 回到 LOBBY。只有 GAME 狀態會送出移動與炸彈，其他狀態下按遊戲鍵不會有任何送出。

## 例外狀況處理

| 狀況 | 處理方式 |
|---|---|
| Client 異常斷線 | client thread 的 `recv()` 回傳 0 或錯誤，送出「斷線」事件給房間；房間判定該玩家出局並通知其他人，處理完才釋放連線資源 |
| Client 當機但沒有關閉連線 | Client 每 5 秒送 PING；Server 以 `SO_RCVTIMEO` 設 15 秒逾時，超過就視為斷線 |
| Server 當機或網路中斷 | Client 超過 15 秒沒收到任何資料就顯示斷線訊息並正常結束 |
| Client 按 Ctrl+D | raw mode 下讀到位元組 0x04，送出 QUIT 後還原終端機並結束 |
| Client 按 Ctrl+C | SIGINT handler 設旗標，主迴圈送出 QUIT 後還原終端機並結束 |
| Server 按 Ctrl+C | 停止接受連線，對所有 Client 送 SHUTDOWN，Client 顯示「伺服器已關閉」 |
| 對已關閉的 socket 寫入 | `send(..., MSG_NOSIGNAL)` 與忽略 SIGPIPE，避免程式被訊號終止 |
| 部分送出 / 部分接收 | `send_all` 迴圈送到完；接收端累積緩衝後以 `\n` 切行 |
| 單行過長 | 超過緩衝區仍沒有換行，Server 直接斷開該連線 |
| 送給慢速 Client 卡住 | `SO_SNDTIMEO` 設 3 秒；送出失敗時 `shutdown()` 該 socket，走正常斷線流程 |
| 格式錯誤、未知代碼、狀態不符 | 回傳對應 ERR；格式錯誤累積超過 20 次則踢除 |
| 狀態不一致 | 序號跳號或雜湊不符時 Client 自動 RESYNC |
| 倒數中有人離開或取消準備 | 倒數取消，回到 WAITING |
| 即時遊戲的延遲 | 兩端都開啟 `TCP_NODELAY`，關閉 Nagle 演算法 |

## 測試

```bash
make
python3 tests/protocol_test.py     # 模擬多個 Client，測試協定、狀態表與例外處理（47 項）
python3 tests/client_pty_test.py   # 以虛擬終端機操作真正的 C Client 玩一局（21 項）
```

`protocol_test.py` 中的測試 Client 會自己維護一份鏡像遊戲狀態，每秒與 Server 的 HASH 比對，用來驗證事件流完整。兩個測試也在 ThreadSanitizer 與 AddressSanitizer 編譯下跑過，沒有資料競爭與記憶體錯誤。

## 建議分工

| 組員 A（Server） | 組員 B（Client） | 共同 |
|---|---|---|
| `server/main.c`、`server/room.c`：執行緒、事件佇列、狀態表、遊戲模擬、斷線與逾時 | `client/core.c`、`client/main.c`、`client/ui.c`、`client/gui.c`：select 迴圈、本地檢查、輸入緩衝、狀態同步、終端機與圖形畫面 | `common/`：協定、遊戲規則、網路工具 |
