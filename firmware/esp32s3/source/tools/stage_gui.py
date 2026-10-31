#!/usr/bin/env python3
"""Bring-up GUI + mock TFT (sync via TCP status). No real panel needed."""

from __future__ import annotations

import re
import socket
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk

DEFAULT_IP = "192.168.110.119"
DEFAULT_PORT = 2333
NK_MAGIC = 0xA5
NK_TEXT = 0x12
KEY_LABELS = [f"K{i}" for i in range(1, 16)] + ["ENC"]

RE_WHO = re.compile(
    r"CH32 WHOAMI=(0x[0-9A-Fa-f]+)\s+VER=(0x[0-9A-Fa-f]+).*ok=(\d).*mask=(0x[0-9A-Fa-f]+).*boost=(\d)",
    re.I,
)
RE_EVT = re.compile(r"EVT\s+(K(\d+)|ENC_SW)\s+(DOWN|UP)", re.I)
RE_ENC1 = re.compile(r"ENC1(?: RIGHT)? delta=(-?\d+)")
RE_ENC2 = re.compile(r"ENC2 vol=(\d+)")
RE_KV = re.compile(r"^([A-Za-z0-9_]+)=(.*)$")


def _xor(data: bytes) -> int:
    x = 0
    for b in data:
        x ^= b
    return x & 0xFF


class StageClient:
    def __init__(self) -> None:
        self.sock: socket.socket | None = None
        self.lock = threading.Lock()
        self._rx = bytearray()
        self.on_text = None

    @property
    def connected(self) -> bool:
        return self.sock is not None

    def connect(self, host: str, port: int) -> None:
        self.close()
        s = socket.create_connection((host, port), timeout=5)
        s.settimeout(0.2)
        self.sock = s
        self._rx.clear()
        threading.Thread(target=self._rx_loop, daemon=True).start()

    def close(self) -> None:
        s = self.sock
        self.sock = None
        if s:
            try:
                s.close()
            except OSError:
                pass

    def send_line(self, line: str) -> None:
        with self.lock:
            if not self.sock:
                raise OSError("not connected")
            self.sock.sendall((line.strip() + "\n").encode("utf-8"))

    def _emit(self, text: str) -> None:
        cb = self.on_text
        if cb and text:
            cb(text)

    def _rx_loop(self) -> None:
        while self.sock:
            try:
                chunk = self.sock.recv(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            self._rx.extend(chunk)
            self._drain()

    def _drain(self) -> None:
        buf = self._rx
        while buf:
            if buf[0] == NK_MAGIC:
                if len(buf) < 3:
                    return
                body = buf[1] | (buf[2] << 8)
                need = 3 + body + 1
                if body < 1 or body > 4096:
                    buf.pop(0)
                    continue
                if len(buf) < need:
                    return
                payload = bytes(buf[3 : 3 + body])
                chk = buf[3 + body]
                del buf[:need]
                if _xor(payload) != chk:
                    continue
                typ, data = payload[0], payload[1:]
                if typ == NK_TEXT:
                    self._emit(data.decode("utf-8", errors="replace"))
                continue
            nl = buf.find(b"\n")
            if nl < 0:
                if len(buf) > 4096:
                    self._emit(bytes(buf).decode("utf-8", errors="replace"))
                    buf.clear()
                return
            line = bytes(buf[:nl]).decode("utf-8", errors="replace").rstrip("\r")
            del buf[: nl + 1]
            if line:
                self._emit(line + "\n")


class StageGui(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("SKY-music 模拟屏 / 音量 / 播放")
        self.geometry("1360x780")
        self.minsize(1100, 640)

        self.client = StageClient()
        self.client.on_text = self._on_text
        self.key_btns: list[tk.Label] = []
        self._auto = tk.BooleanVar(value=True)
        self._ui_q: list[str] = []
        self._q_lock = threading.Lock()
        self._status: dict[str, str] = {}
        self._in_status = False
        self._status_buf: dict[str, str] = {}
        self._enc1 = 0

        self._build()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(80, self._tick)
        self.after(400, self._poll)

    def _build(self) -> None:
        top = ttk.Frame(self, padding=8)
        top.pack(fill=tk.X)
        ttk.Label(top, text="IP").pack(side=tk.LEFT)
        self.ip_var = tk.StringVar(value=DEFAULT_IP)
        ttk.Entry(top, textvariable=self.ip_var, width=16).pack(side=tk.LEFT, padx=4)
        self.port_var = tk.StringVar(value=str(DEFAULT_PORT))
        ttk.Entry(top, textvariable=self.port_var, width=6).pack(side=tk.LEFT, padx=4)
        self.conn_btn = ttk.Button(top, text="连接", command=self._toggle)
        self.conn_btn.pack(side=tk.LEFT, padx=6)
        ttk.Checkbutton(top, text="轮询 status", variable=self._auto).pack(side=tk.LEFT, padx=8)
        self.link_lbl = ttk.Label(top, text="未连接", foreground="#a33")
        self.link_lbl.pack(side=tk.LEFT, padx=8)

        ttk.Label(
            self,
            text="假屏=真屏三区骨架。转=浏览 短按=确认 长按=切模式。大库索引 n/total；底栏进度。",
            padding=4,
        ).pack(fill=tk.X)

        body = ttk.Frame(self, padding=(8, 0))
        body.pack(fill=tk.BOTH, expand=True)

        left = ttk.Frame(body)
        left.pack(side=tk.LEFT, fill=tk.Y)

        libs = ttk.Frame(body)
        libs.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=8)
        mf = ttk.LabelFrame(libs, text="/sdcard/music", padding=4)
        mf.pack(fill=tk.BOTH, expand=True)
        self.lst_m = tk.Listbox(mf, height=12, font=("Consolas", 9), exportselection=False)
        self.lst_m.pack(fill=tk.BOTH, expand=True)
        sf = ttk.LabelFrame(libs, text="/sdcard/puzi", padding=4)
        sf.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        self.lst_s = tk.Listbox(sf, height=12, font=("Consolas", 9), exportselection=False)
        self.lst_s.pack(fill=tk.BOTH, expand=True)

        tft_f = ttk.LabelFrame(left, text="模拟屏 320×172 横置（2×显示）", padding=6)
        tft_f.pack(fill=tk.X)
        self.tft = tk.Canvas(tft_f, width=640, height=344, bg="#101418", highlightthickness=1, highlightbackground="#333")
        self.tft.pack()
        self._draw_tft()

        kg = ttk.LabelFrame(left, text="矩阵", padding=6)
        kg.pack(fill=tk.X, pady=6)
        grid = ttk.Frame(kg)
        grid.pack()
        for r in range(4):
            for c in range(4):
                idx = r * 4 + c
                lbl = tk.Label(
                    grid,
                    text=KEY_LABELS[idx],
                    width=4,
                    height=1,
                    relief=tk.RAISED,
                    bd=2,
                    bg="#e8e8e8",
                    font=("Segoe UI", 8, "bold"),
                )
                lbl.grid(row=r, column=c, padx=1, pady=1)
                self.key_btns.append(lbl)

        vol = ttk.LabelFrame(left, text="左手 ENC2 音量（也可点这里）", padding=6)
        vol.pack(fill=tk.X)
        for i, (t, fn) in enumerate(
            [("Vol −", lambda: self._nudge_vol(-1)), ("Mute", self._mute), ("Vol +", lambda: self._nudge_vol(1))]
        ):
            ttk.Button(vol, text=t, command=fn).grid(row=0, column=i, padx=3, sticky=tk.EW)

        fn = ttk.LabelFrame(left, text="右手 ENC1（旋钮或点这里）", padding=6)
        fn.pack(fill=tk.X, pady=6)
        for i, (t, c) in enumerate(
            [
                ("ENC1 −", "enc1 -"),
                ("短按", "enc1 click"),
                ("ENC1 +", "enc1 +"),
                ("长按切模式", "enc1 long"),
            ]
        ):
            ttk.Button(fn, text=t, command=lambda x=c: self._send(x)).grid(
                row=0 if i < 3 else 1, column=i if i < 3 else 0, padx=2, pady=2, sticky=tk.EW
            )

        cmdf = ttk.Frame(left, padding=(0, 6))
        cmdf.pack(fill=tk.X)
        self.cmd_var = tk.StringVar()
        ent = ttk.Entry(cmdf, textvariable=self.cmd_var)
        ent.pack(side=tk.LEFT, fill=tk.X, expand=True)
        ent.bind("<Return>", lambda _e: self._send(self.cmd_var.get()))
        ttk.Button(cmdf, text="发送", command=lambda: self._send(self.cmd_var.get())).pack(side=tk.LEFT, padx=4)

        right = ttk.Frame(body)
        right.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=(8, 0))
        ttk.Label(right, text="log").pack(anchor=tk.W)
        self.log = scrolledtext.ScrolledText(right, height=32, width=52, font=("Consolas", 9))
        self.log.pack(fill=tk.BOTH, expand=True)

    def _draw_tft(self) -> None:
        c = self.tft
        c.delete("all")
        st = self._status
        S = 2  # 320x172 landscape, 2× on PC
        vol = int(st.get("master", "0") or 0)
        bank = st.get("bank_name", "—")
        mode = st.get("enc1_mode", "play")
        sel = st.get("enc1_sel", "—")
        idx = st.get("enc1_idx", "0")
        n = st.get("enc1_n", "0")
        voices = st.get("voices", "0")
        sp, spa = st.get("score_play", "0"), st.get("score_pause", "0")
        if spa == "1":
            sstate = "PAUSE"
        elif sp == "1":
            sstate = "PLAY"
        else:
            sstate = "STOP"
        mp = "PLAY" if st.get("music_play", "0") == "1" else "—"

        def t(x, y, text, fill, size=10, anchor="nw", bold=False):
            font = ("Consolas", size, "bold" if bold else "normal")
            c.create_text(x * S, y * S, text=text, fill=fill, font=font, anchor=anchor)

        c.create_rectangle(0, 0, 320 * S, 172 * S, fill="#101418", outline="#234")
        prev = st.get("enc1_prev", "—")
        nxt = st.get("enc1_next", "—")
        stub = st.get("enc1_stub", "0") == "1"
        t(8, 6, "SKY-music  320x172", "#7ab", 8)
        chg = st.get("chg", "—")
        soc = st.get("bat_soc", "—")
        mv = st.get("bat_mv", "0")
        bok = st.get("bat_ok", "0") == "1"
        t(148, 6, f"{mv}mV {soc}% {chg}" if bok else f"BAT — {chg}", "#8c8" if chg == "charging" else "#9ab", 8)
        t(8, 22, f"MODE {mode.upper()}", "#fc6" if not stub else "#c84", 12, bold=True)
        t(200, 24, "(stub)" if stub else f"{idx}/{n}", "#888", 9)
        t(16, 44, prev, "#556", 9)
        t(16, 60, f"> {sel}", "#eee", 11, bold=True)
        t(16, 78, nxt, "#556", 9)
        t(8, 98, "VOL", "#9aa", 9)
        x0, y0, x1, y1 = 40, 96, 250, 112
        c.create_rectangle(x0 * S, y0 * S, x1 * S, y1 * S, outline="#456", fill="#1a2228")
        fw = int((x1 - x0) * max(0, min(100, vol)) / 100)
        c.create_rectangle(x0 * S, y0 * S, (x0 + fw) * S, y1 * S, outline="", fill="#3c8")
        t(255, 98, str(vol), "#8f8", 10)
        need = st.get("enc1_need_ok", "0") == "1"
        mode_l = mode.lower()
        try:
            if mode_l == "score":
                pct = int(st.get("score_pct", "0") or 0)
            else:
                pct = int(st.get("music_pct", "0") or 0)
        except ValueError:
            pct = 0
        pct = max(0, min(100, pct))
        t(8, 118, f"BANK {bank}  SCORE {sstate}  MUSIC {mp}", "#9ab", 8)
        tip = "点确认加载" if need else "L音量  R浏览/确认  长按模式"
        t(8, 134, tip, "#fc6" if need else "#678", 8)
        bx0, by0, bx1, by1 = 8, 150, 250, 164
        c.create_rectangle(bx0 * S, by0 * S, bx1 * S, by1 * S, outline="#456", fill="#1a2228")
        fw = int((bx1 - bx0) * pct / 100)
        c.create_rectangle(bx0 * S, by0 * S, (bx0 + fw) * S, by1 * S, outline="", fill="#48a")
        t(258, 150, f"{pct}%", "#9ab", 8)

    def _vol_now(self) -> int:
        try:
            return int(self._status.get("master", "20") or 20)
        except ValueError:
            return 20

    def _nudge_vol(self, d: int) -> None:
        v = max(0, min(100, self._vol_now() + d))
        self._send(f"master {v}")

    def _mute(self) -> None:
        if self._vol_now() == 0:
            self._send("master 20")
        else:
            self._send("master 0")

    def _toggle(self) -> None:
        if self.client.connected:
            self.client.close()
            self.conn_btn.config(text="连接")
            self.link_lbl.config(text="未连接", foreground="#a33")
            return
        host = self.ip_var.get().strip()
        try:
            port = int(self.port_var.get().strip())
            self.client.connect(host, port)
        except Exception as exc:
            messagebox.showerror("连接失败", str(exc))
            return
        self.conn_btn.config(text="断开")
        self.link_lbl.config(text=f"已连 {host}:{port}", foreground="#181")
        self._send("status")

    def _send(self, line: str) -> None:
        line = (line or "").strip()
        if not line:
            return
        try:
            self.client.send_line(line)
            self._append(f">> {line}\n")
        except OSError as exc:
            self._append(f"!! {exc}\n")

    def _on_text(self, text: str) -> None:
        with self._q_lock:
            self._ui_q.append(text)

    def _append(self, text: str) -> None:
        if "BEGIN_STATUS" in text or text.startswith("rate=") or text.startswith("slot="):
            return
        if text.startswith("heap_") or text.startswith("psram_") or text.startswith("sd_pins") or text.startswith("lib_"):
            return
        self.log.insert(tk.END, text)
        self.log.see(tk.END)

    def _paint_mask(self, mask: int) -> None:
        for i, lbl in enumerate(self.key_btns):
            on = bool(mask & (1 << i))
            lbl.config(bg="#6d6" if on else "#e8e8e8", relief=tk.SUNKEN if on else tk.RAISED)

    def _refresh_libs(self) -> None:
        st = self._status
        mode = st.get("enc1_mode", "")
        try:
            cur = int(st.get("enc1_idx", "0") or 0) - 1
        except ValueError:
            cur = -1

        def fill(box: tk.Listbox, prefix: str, nkey: str, active: bool) -> None:
            try:
                n = int(st.get(nkey, "0") or 0)
            except ValueError:
                n = 0
            names = [st.get(f"{prefix}{i}", "") for i in range(n)]
            old = list(box.get(0, tk.END))
            if not any(names) and old:
                names = old
            if old != names:
                box.delete(0, tk.END)
                for name in names:
                    box.insert(tk.END, name)
            box.selection_clear(0, tk.END)
            if active and 0 <= cur < box.size():
                box.selection_set(cur)
                box.see(cur)

        fill(self.lst_m, "lib_m", "lib_mn", mode == "music")
        fill(self.lst_s, "lib_s", "lib_sn", mode == "score")

    def _commit_status(self) -> None:
        self._status = dict(self._status_buf)
        mask = self._status.get("keymask", "")
        if mask.startswith("0x"):
            try:
                self._paint_mask(int(mask, 16))
            except ValueError:
                pass
        self._draw_tft()
        self._refresh_libs()

    def _apply(self, text: str) -> None:
        for raw in text.splitlines():
            line = raw.strip()
            if line == "BEGIN_STATUS":
                self._in_status = True
                self._status_buf = {}
                continue
            if line == "END_STATUS":
                self._in_status = False
                self._commit_status()
                continue
            if self._in_status:
                m = RE_KV.match(line)
                if m:
                    self._status_buf[m.group(1)] = m.group(2)
                continue
            self._append(line + "\n")
            m = RE_WHO.search(line)
            if m:
                try:
                    self._paint_mask(int(m.group(4), 16))
                except ValueError:
                    pass
            for ev in RE_EVT.finditer(line):
                if ev.group(1).upper() == "ENC_SW":
                    idx = 15
                else:
                    idx = int(ev.group(2)) - 1
                down = ev.group(3).upper() == "DOWN"
                if 0 <= idx < len(self.key_btns):
                    self.key_btns[idx].config(
                        bg="#6d6" if down else "#e8e8e8",
                        relief=tk.SUNKEN if down else tk.RAISED,
                    )
            m = RE_ENC1.search(line)
            if m:
                self._enc1 += int(m.group(1))
            m = RE_ENC2.search(line)
            if m:
                self._status["master"] = m.group(1)
                self._draw_tft()

    def _tick(self) -> None:
        batch: list[str] = []
        with self._q_lock:
            batch, self._ui_q = self._ui_q, []
        for t in batch:
            self._apply(t)
        self.after(80, self._tick)

    def _poll(self) -> None:
        if self.client.connected and self._auto.get():
            try:
                self.client.send_line("status")
            except OSError:
                pass
        self.after(1500, self._poll)

    def _on_close(self) -> None:
        self.client.close()
        self.destroy()


def main() -> None:
    StageGui().mainloop()


if __name__ == "__main__":
    main()
