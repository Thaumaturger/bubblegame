#!/usr/bin/env python3
"""
協定自動測試：模擬多個 Client 連上 Server，檢查各種狀態 × 訊息組合與例外處理。
每個測試 Bot 都維護一份鏡像遊戲狀態，套用事件後與 Server 送來的 HASH 比對，
用來驗證「事件流完整、Client 與 Server 狀態一致」。

用法：python3 tests/protocol_test.py   （會自動啟動 ./bubble_server）
"""
import os, random, signal, socket, subprocess, sys, time

PORT = 7799
W, H = 15, 13
SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "bubble_server")
DX = [0, 0, 0, -1, 1]; DY = [0, -1, 1, 0, 0]
PS_EMPTY, PS_ALIVE, PS_TRAPPED, PS_DEAD = 0, 1, 2, 3

passed = 0
def ok(cond, msg):
    global passed
    if not cond:
        print("  \033[91mFAIL\033[0m", msg); sys.exit(1)
    passed += 1
    print("  \033[92mok\033[0m  ", msg)

def fnv(vals):
    h = 2166136261
    for v in vals:
        v &= 0xffffffff
        for k in range(4):
            h ^= (v >> (k * 8)) & 0xff
            h = (h * 16777619) & 0xffffffff
    return h

class Bot:
    def __init__(self, name=None):
        self.s = socket.create_connection(("127.0.0.1", PORT))
        self.s.settimeout(0.05)
        self.buf = b""; self.lines = []
        self.seq = 0; self.synced = False; self.hash_checks = 0
        self.seat = -1; self.over = None; self.closed = False
        if name: self.hello(name)

    def send(self, line): self.s.sendall((line + "\n").encode())

    def pump(self, t=0.05):
        if self.closed: return
        end = time.time() + t
        while time.time() < end:
            try:
                d = self.s.recv(65536)
                if not d: self.lines.append("<EOF>"); self.closed = True; return
                self.buf += d
                while b"\n" in self.buf:
                    l, self.buf = self.buf.split(b"\n", 1)
                    self.handle(l.decode())
            except socket.timeout:
                pass
            except (ConnectionResetError, OSError):
                self.lines.append("<EOF>"); self.closed = True; return

    def wait(self, code, timeout=3.0, pred=None):
        end = time.time() + timeout
        while time.time() < end:
            for i, l in enumerate(self.lines):
                p = l.split()
                if p and p[0] == str(code) and (pred is None or pred(p)):
                    del self.lines[:i + 1]
                    return p
            self.pump(0.05)
        return None

    def hello(self, name):
        self.send(f"100 {name}")
        assert self.wait(500), "no welcome"

    # ---- 鏡像狀態 ----
    def snapshot(self, p):
        self.seq = int(p[1]); self.mode = int(p[3]); m = p[4]
        self.map = [list(m[y * W:(y + 1) * W]) for y in range(H)]
        self.pl = [list(map(int, p[5 + i].split(","))) for i in range(4)]
        nb = int(p[9]); self.bombs = [list(map(int, x.split(","))) for x in p[10:10 + nb]]
        self.synced = True

    def bomb_at(self, x, y): return any(b[0] == x and b[1] == y for b in self.bombs)

    def myhash(self):
        vals = []
        for y in range(H):
            for x in range(W):
                vals += [ord(self.map[y][x]), int(self.bomb_at(x, y))]
        for p in self.pl: vals += p[:6]
        return fnv(vals)

    def apply(self, code, a):
        if code == 540: s, x, y = map(int, a[:3]); self.pl[s][1], self.pl[s][2] = x, y
        elif code == 541: self.bombs.append(list(map(int, a[:5])))
        elif code == 542:
            x, y = int(a[0]), int(a[1]); self.bombs = [b for b in self.bombs if not (b[0] == x and b[1] == y)]
        elif code == 543: self.map[int(a[1])][int(a[0])] = a[2]
        elif code == 544:
            s, x, y, it = int(a[0]), int(a[1]), int(a[2]), a[3]; p = self.pl[s]
            if it == 'b' and p[3] < 6: p[3] += 1
            if it == 'r' and p[4] < 6: p[4] += 1
            if it == 's' and p[5] < 3: p[5] += 1
            self.map[y][x] = '.'
        elif code == 545: self.pl[int(a[0])][0] = PS_TRAPPED
        elif code == 546: self.pl[int(a[0])][0] = PS_ALIVE
        elif code == 547: self.pl[int(a[0])][0] = PS_DEAD
        elif code == 560: self.over = (int(a[0]), int(a[1]))

    def handle(self, line):
        p = line.split(); code = int(p[0])
        if code == 550: self.snapshot(p); self.lines.append(line); return
        if 540 <= code <= 547 or code == 560:
            if self.synced:
                seq = int(p[1])
                assert seq == self.seq + 1, f"seq gap {self.seq}->{seq}"
                self.seq = seq; self.apply(code, p[2:])
        if code == 551 and self.synced and int(p[1]) == self.seq:
            assert self.myhash() == int(p[2]), "HASH MISMATCH"
            self.hash_checks += 1
        if code == 511: self.seat = int(p[2])
        self.lines.append(line)

    def can_move(self, d):
        p = self.pl[self.seat]
        if p[0] != PS_ALIVE: return False
        x, y = p[1] + DX[d], p[2] + DY[d]
        return 0 <= x < W and 0 <= y < H and self.map[y][x] not in "#+" and not self.bomb_at(x, y)

    def close(self): self.s.close()

def start_server():
    return subprocess.Popen([SERVER, str(PORT)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

def make_game(names, mode=0):
    bots = [Bot(n) for n in names]
    bots[0].send(f"111 {mode}")
    j = bots[0].wait(511); rid = int(j[1])
    for b in bots[1:]:
        b.send(f"112 {rid}"); b.wait(511)
    for b in bots: b.send("120 1")
    for b in bots: assert b.wait(522, 5), "no start"
    for b in bots: assert b.wait(550, 2), "no snapshot"
    return rid, bots

def main():
    srv = start_server(); time.sleep(0.3)
    try:
        print("\n[1] 連線與登入")
        c = Bot()
        c.send("110"); p = c.wait(700); ok(p and p[1] == "3", "未登入時查詢房間 → ERR 3 (狀態錯誤)")
        c.send("100 bad:nick"); p = c.wait(700); ok(p and p[1] == "7", "暱稱含冒號 → ERR 7")
        c.send("hello"); p = c.wait(700); ok(p and p[1] == "1", "非數字代碼 → ERR 1 (格式錯誤)")
        c.send("100 小明"); p = c.wait(500); ok(p is not None, "中文暱稱登入成功 → WELCOME")
        c.send("555"); p = c.wait(700); ok(p and p[1] == "2", "未知代碼 → ERR 2")
        c.send("900"); ok(c.wait(901) is not None, "PING → PONG")

        print("\n[2] 大廳與房間")
        c.send("110"); p = c.wait(510); ok(p and p[1] == "0", "初始房間列表為空")
        c.send("112 3"); p = c.wait(700); ok(p and p[1] == "4", "加入不存在的房間 → ERR 4")
        c.send("200 U"); p = c.wait(700); ok(p and p[1] == "3", "大廳中移動 → ERR 3")
        c.send("111 0"); p = c.wait(511); ok(p and p[2] == "0", "建立房間，成為第 1 位玩家 (seat 0)")
        rid = int(p[1])
        d = Bot("阿華"); d.send(f"112 {rid}"); p = d.wait(511)
        ok(p and p[2] == "1", "第二人加入，成為第 2 位玩家 (seat 1)")
        p = c.wait(512); ok(p is not None, "房主收到成員名單更新 (ROSTER)")
        d.send("201"); p = d.wait(700); ok(p and p[1] == "3", "等待中放炸彈 → ERR 3")
        d.send("111 0"); p = d.wait(700); ok(p and p[1] == "3", "房間內再建房 → ERR 3")
        d.send("210 大家好 hello"); p = c.wait(600); ok(p and " ".join(p[2:]) == "大家好 hello", "聊天廣播")
        c.send("120 1"); d.send("120 1")
        p = c.wait(520); ok(p and p[1] == "3", "全員準備 → 倒數 3 秒")
        d.send("120 0"); p = c.wait(521); ok(p is not None, "倒數中取消準備 → CANCEL")
        e = Bot("路人"); e.send("110"); p = e.wait(510); ok(p and p[1] == "1", "新玩家看到 1 個房間")
        c.send("120 1"); d.send("120 1")
        ok(c.wait(522, 5) is not None and c.wait(550) is not None, "遊戲開始並收到完整快照")
        d.wait(522); d.wait(550)
        e.send(f"112 {rid}"); p = e.wait(700); ok(p and p[1] == "6", "遊戲中加入 → ERR 6")
        c.send("120 1"); p = c.wait(700); ok(p and p[1] == "3", "遊戲中送 READY → ERR 3")

        print("\n[3] 遊戲進行：隨機操作 10 秒，驗證序號連續與狀態雜湊一致")
        bots = [c, d]; end = time.time() + 10; sent = 0
        while time.time() < end and not any(b.over for b in bots):
            for b in bots:
                b.pump(0.02)
                if b.synced and b.pl[b.seat][0] == PS_ALIVE:
                    dirs = [k for k in range(1, 5) if b.can_move(k)]
                    if dirs: b.send(f"200 {'?UDLR'[random.choice(dirs)]}"); sent += 1
                    if random.random() < 0.08: b.send("201")
        for b in bots: b.pump(0.3)
        ok(c.hash_checks >= 5 and d.hash_checks >= 5,
           f"雜湊比對全部一致（{c.hash_checks}+{d.hash_checks} 次），共送出 {sent} 個移動")

        print("\n[4] 再玩一局，遊戲中異常斷線")
        if not any(b.over for b in bots):
            c.send("290")                                  # 先結束這局
        p = c.wait(523, 8); ok(p is not None, "結算 5 秒後回到房間 (BACK)")
        d.wait(523, 2)
        for b in bots: b.synced = False; b.over = None; b.lines.clear()
        c.send("120 1"); d.send("120 1")
        ok(c.wait(522, 5) is not None and c.wait(550) is not None, "同一房間開始第二局")
        d.s.close()                                        # 模擬 Client 當機
        p = c.wait(547, 3, lambda p: p[2] == "1"); ok(p and p[3] == "3", "對手斷線 → 判定出局 (DIED reason=3)")
        p = c.wait(513, 3); ok(p and p[2] == "1", "收到對手斷線通知 (LEFT reason=1)")
        p = c.wait(560, 3); ok(p and p[2] == "1" and p[3] == "0", "剩下的玩家獲勝 (GAMEOVER)")
        p = c.wait(523, 7); ok(p is not None, "5 秒後回到房間 (BACK)")
        c.send("113"); p = c.wait(513); ok(p and p[1] == "0", "離開房間")
        f = Bot("觀察者"); f.send("110"); p = f.wait(510); ok(p and p[1] == "0", "最後一人離開後房間關閉")
        p = e.wait(901, 0.5); e.pump(0.2); ok("<EOF>" in e.lines, "閒置超過 15 秒未送心跳的連線被伺服器踢除")

        print("\n[5] 房間人數上限")
        bs = [Bot(f"p{i}") for i in range(5)]
        bs[0].send("111 0"); rid = int(bs[0].wait(511)[1])
        for b in bs[1:4]: b.send(f"112 {rid}"); b.wait(511)
        bs[4].send(f"112 {rid}"); p = bs[4].wait(700); ok(p and p[1] == "5", "第 5 人加入 → ERR 5 (房間已滿)")
        for b in bs: b.close()
        time.sleep(0.3)

        print("\n[6] 認輸與團隊戰")
        rid, (a, b) = make_game(["甲", "乙"], mode=1)
        ok(a.mode == 1, "團隊戰開始")
        a.send("290"); p = b.wait(560, 3); ok(p and p[2] == "2" and p[3] == "1", "A 隊認輸 → B 隊獲勝")
        a.send("290"); p = a.wait(700); ok(p and p[1] == "3", "結算中認輸 → ERR 3")
        a.close(); b.close(); time.sleep(0.3)

        print("\n[7] 炸彈、方塊、道具、泡泡")
        rid, (a, b) = make_game(["炸", "彈"])
        a.send("201"); p = a.wait(541); ok(p is not None, "放炸彈 (BOMB 事件)")
        a.send("201"); p = a.wait(700); ok(p and p[1] == "9", "超過炸彈上限 → ERR 9")
        p = a.wait(542, 4); ok(p is not None, "3 秒後爆炸 (EXPLODE 事件)")
        p = a.wait(545, 1, lambda p: p[2] == "0"); ok(p is not None, "站在原地的玩家被泡泡困住 (TRAPPED)")
        a.send("200 R"); a.pump(0.3); ok(a.pl[0][1] == 1, "被困住時無法移動")
        p = a.wait(547, 6, lambda p: p[2] == "0"); ok(p and p[3] == "0", "5 秒後泡泡破裂出局")
        p = b.wait(560, 2); ok(p and p[3] == "1", "對手獲勝")
        a.pump(0.2); ok(a.hash_checks > 0, f"全程雜湊一致（{a.hash_checks} 次）")
        a.close(); b.close(); time.sleep(0.3)

        print("\n[8] 惡意 / 錯誤輸入")
        x = Bot("壞人"); x.send("A" * 9000)
        x.pump(0.5); ok("<EOF>" in x.lines, "超長訊息 → 伺服器斷開連線")
        y = Bot("y"); y.send("299"); y.pump(0.3); ok("<EOF>" in y.lines, "QUIT → 伺服器關閉連線")

        print("\n[9] 伺服器關閉")
        z = Bot("z"); srv.send_signal(signal.SIGINT)
        p = z.wait(799, 3); ok(p is not None, "Ctrl+C 關閉伺服器 → 通知所有 Client (SHUTDOWN)")
        srv.wait(3); ok(srv.returncode == 0, "伺服器正常結束")
    finally:
        if srv.poll() is None: srv.kill()
    print(f"\n\033[92m全部通過：{passed} 項\033[0m")

if __name__ == "__main__":
    main()
