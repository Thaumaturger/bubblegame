#!/usr/bin/env python3
"""
以虛擬終端機（pty）實際操作兩個 C Client 進行一局遊戲，
並用簡易終端模擬器把畫面還原成文字，檢查介面與狀態同步是否正常。

用法：python3 tests/client_pty_test.py
"""
import os, pty, re, select, signal, subprocess, sys, time, unicodedata

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PORT = "7798"

class Screen:
    """極簡 ANSI 終端模擬器：支援游標定位、清除、UTF-8 全形字"""
    def __init__(self, rows=24, cols=80):
        self.R, self.C = rows, cols
        self.clear(); self.r = self.c = 0; self.pend = b""
    def clear(self): self.g = [[" "] * self.C for _ in range(self.R)]
    def feed(self, data):
        s = (self.pend + data); self.pend = b""
        try: t = s.decode()
        except UnicodeDecodeError as e:
            t = s[:e.start].decode(); self.pend = s[e.start:]
        i = 0
        while i < len(t):
            ch = t[i]
            if ch == "\x1b":
                m = re.match(r"\x1b\[([?0-9;]*)([A-Za-z])", t[i:])
                if not m:
                    if len(t) - i < 16: self.pend = t[i:].encode(); return   # 不完整的序列，等下一段
                    i += 1; continue
                args, cmd = m.group(1), m.group(2); i += m.end()
                if cmd == "H":
                    p = [int(x) if x else 1 for x in args.split(";")] if args else [1, 1]
                    self.r, self.c = p[0] - 1, (p[1] if len(p) > 1 else 1) - 1
                elif cmd == "K":
                    for x in range(self.c, self.C): self.g[self.r][x] = " "
                elif cmd == "J" and args == "2": self.clear()
                continue
            w = 2 if unicodedata.east_asian_width(ch) in "WF" else 1
            if 0 <= self.r < self.R and self.c + w <= self.C:
                self.g[self.r][self.c] = ch
                if w == 2: self.g[self.r][self.c + 1] = ""
            self.c += w; i += 1
    def text(self): return "\n".join("".join(r).rstrip() for r in self.g)

class Term:
    def __init__(self):
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["TERM"] = "xterm"
            os.execv(os.path.join(ROOT, "bubble_client"), ["bubble_client", "127.0.0.1", PORT])
        self.pid, self.fd, self.scr = pid, fd, Screen()
    def pump(self, t=0.2):
        end = time.time() + t
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.05)
            if r:
                try: d = os.read(self.fd, 65536)
                except OSError: return False
                if not d: return False
                self.scr.feed(d)
        return True
    def type(self, s, t=0.3):
        os.write(self.fd, s.encode()); self.pump(t)
    def has(self, s): return s in self.scr.text()

passed = 0
def ok(c, m):
    global passed
    if not c: print("  \033[91mFAIL\033[0m", m); return False
    passed += 1; print("  \033[92mok\033[0m  ", m); return True

def main():
    srv = subprocess.Popen([os.path.join(ROOT, "bubble_server"), PORT], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.3)
    a, b = Term(), Term()
    a.pump(0.5); b.pump(0.5)
    ok(a.has("請輸入暱稱"), "啟動後顯示輸入暱稱提示")
    a.type("bad name\r"); ok(a.has("不可含空白"), "本地檢查：暱稱含空白被擋下（未送到伺服器）")
    a.type("小美\r"); b.type("大雄\r")
    ok(a.has("目前沒有房間"), "登入後進入大廳並顯示房間列表")
    a.type("/join 5\r"); ok(a.has("找不到房間 #5"), "本地檢查：加入不存在的房間被擋下")
    a.type("/create\r", 0.5); ok(a.has("你是第 1 位加入的玩家"), "建立房間，提示為第 1 位玩家並等待")
    b.type("/list\r"); ok(b.has("#1") and b.has("1/4"), "另一位玩家看到房間 #1 (1/4)")
    b.type("/join 1\r", 0.5); ok(b.has("你是第 2 位加入的玩家"), "加入房間，提示為第 2 位玩家")
    a.type("哈囉\r", 0.4); b.pump(0.3); ok(b.has("[小美] 哈囉"), "房間聊天")
    a.type("/ready\r"); b.type("/ready\r", 0.3); a.pump(0.3)
    ok(a.has("遊戲將在"), "全員準備後開始倒數")
    a.pump(3.5); b.pump(0.3)
    ok(a.has("遊戲開始") and a.has("剩餘時間"), "進入遊戲畫面")
    print("\n--- 小美的畫面 ---\n" + a.scr.text() + "\n------------------")

    # 開局在角落 (1,1)：右邊 (2,1) 與下面 (1,2) 一定是空地，(2,2) 是牆
    a.type("s", 0.4)
    a.type("s", 0.1)                          # (1,3) 可能是方塊，被本地擋下也沒關係
    a.type("w", 0.4); a.type("w", 0.4); a.type("d", 0.4)
    a.type(" ", 0.3); ok("@" in a.scr.text(), "在 (2,1) 放炸彈，畫面出現炸彈 @")
    a.type(" ", 0.3); ok(a.has("炸彈已用完"), "本地檢查：超過炸彈上限被擋下")
    a.type("a", 0.4); a.type("s", 0.4)       # 躲到 (1,2)，不在火焰範圍內
    for _ in range(14): a.pump(0.25); b.pump(0.01)
    ok(a.has("存活 ←你"), "躲開爆炸，仍然存活")
    b.type("t"); b.type("加油\r", 0.4); a.pump(0.3); ok(a.has("[大雄] 加油"), "遊戲中聊天（T 進入聊天模式）")
    print("\n--- 小美的畫面（爆炸後） ---\n" + a.scr.text() + "\n------------------")

    b.type("x"); ok(b.has("確定要認輸嗎"), "按 X 出現認輸確認")
    b.type("y", 0.8); a.pump(0.5)
    ok(a.has("你贏了") or a.has("小美 獲勝"), "對手認輸 → 小美獲勝")
    ok(b.has("你輸了"), "大雄看到落敗結果")
    a.pump(5.5); ok(a.has("回到房間"), "5 秒後自動回到房間")

    b.type("\x04", 0.5)                      # Ctrl+D
    alive = b.pump(0.5)
    ok(not alive, "Ctrl+D 後 Client 正常結束")
    a.pump(0.5); ok(a.has("大雄 斷線了") or a.has("大雄 離開"), "另一位玩家收到離開通知")

    os.kill(a.pid, signal.SIGINT); a.pump(0.5)
    srv.send_signal(signal.SIGINT); out = srv.communicate(timeout=3)[0]
    ok("resync" not in out, "整局沒有任何 Client 需要重新同步（狀態一致）")
    print("\n--- 伺服器紀錄 ---\n" + out)
    print(f"\033[92m通過 {passed} 項\033[0m")

if __name__ == "__main__":
    main()
