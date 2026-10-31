#!/usr/bin/env python3
"""ESP32-S3 music keyboard serial debug pad."""
from __future__ import annotations

import array
import json
import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import threading
import time
import wave
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from urllib.parse import unquote

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:
    raise SystemExit("请先安装: pip install pyserial") from exc


BAUD = 115200
NK_MAGIC = 0xA5
NK_CMD = 0x01
NK_STATUS = 0x10
NK_SCOPE = 0x11
NK_TEXT = 0x12
NK_STATUS_STRUCT = struct.Struct("<BBBBHHHHHHHIII12s16sB")
# packed MixerScope: 4 i32 + u32 + 2 u16 + 2 u8 + u16 + 64 i16
NK_SCOPE_STRUCT = struct.Struct("<iiiiIHHBBH64h64h")


def nk_cmd_frame(line: str) -> bytes:
    body = bytes([NK_CMD]) + line.encode("utf-8")
    x = 0
    for b in body:
        x ^= b
    ln = len(body)
    return bytes([NK_MAGIC, ln & 0xFF, (ln >> 8) & 0xFF]) + body + bytes([x])



def _win_port(name: str) -> str:
    if name.upper().startswith("COM"):
        try:
            n = int(name[3:])
        except ValueError:
            return name
        if n >= 10:
            return r"\\.\\" + name
    return name


def _read_score_text(path: str) -> str:
    data = open(path, "rb").read()
    if data.startswith(b"\xff\xfe") or data.startswith(b"\xfe\xff"):
        return data.decode("utf-16")
    if data.startswith(b"\xef\xbb\xbf"):
        return data.decode("utf-8-sig")
    for enc in ("utf-8", "gb18030"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    return data.decode("utf-8", errors="replace")


def parse_sky_score(path: str) -> tuple[str, list[tuple[int, int]]]:
    """Return (title, [(time_ms, key_1_to_15), ...]). 1Key/2Key merge; same time+key kept once."""
    raw = _read_score_text(path)
    doc = json.loads(raw)
    song = doc[0] if isinstance(doc, list) and doc else doc
    if not isinstance(song, dict):
        raise ValueError("不是谱子 JSON")
    title = str(song.get("name") or path)
    notes_in = song.get("songNotes") or []
    seen: set[tuple[int, int]] = set()
    out: list[tuple[int, int]] = []
    key_re = re.compile(r"Key(\d+)$", re.I)
    for item in notes_in:
        if not isinstance(item, dict):
            continue
        try:
            t = int(item.get("time", 0))
        except (TypeError, ValueError):
            continue
        m = key_re.search(str(item.get("key", "")))
        if not m:
            continue
        idx = int(m.group(1))
        if idx < 0 or idx > 14:
            continue
        pair = (t, idx)
        if pair in seen:
            continue
        seen.add(pair)
        out.append((t, idx + 1))
    out.sort(key=lambda x: (x[0], x[1]))
    return title, out


MIX_RATE = 22050


def _ffmpeg() -> str:
    p = shutil.which("ffmpeg")
    if not p:
        raise FileNotFoundError("PATH 里找不到 ffmpeg")
    return p


def load_key_pcm(path: str, rate: int = MIX_RATE) -> array.array:
    """ffmpeg 转成 22050 Hz / 16-bit / 单声道，与板上 audio_bank 一致。"""
    ff = _ffmpeg()
    r = subprocess.run(
        [
            ff, "-nostdin", "-v", "error", "-i", path,
            "-f", "s16le", "-acodec", "pcm_s16le",
            "-ac", "1", "-ar", str(rate), "pipe:1",
        ],
        capture_output=True,
        check=False,
    )
    if r.returncode != 0 or not r.stdout:
        err = r.stderr.decode("utf-8", "replace").strip() or "ffmpeg 无输出"
        raise RuntimeError(f"{os.path.basename(path)}: {err[:400]}")
    return array.array("h", r.stdout)


def render_score_acc(
    notes: list[tuple[int, int]],
    bank: list[array.array],
    rate: int = MIX_RATE,
) -> array.array:
    """板上混音：每个音 one-shot 整段叠加进 int32 总线（力度满）。"""
    if not notes:
        raise ValueError("谱子没有音符")
    tail = max((len(p) for p in bank if p), default=0)
    last_ms = notes[-1][0]
    total = int(last_ms * rate / 1000.0) + tail + rate // 2
    if total < rate:
        total = rate
    acc = array.array("i", bytes(total * 4))
    for t_ms, key in notes:
        if key < 1 or key > len(bank):
            continue
        pcm = bank[key - 1]
        if not pcm:
            continue
        pos = int(t_ms * rate / 1000.0)
        n = len(pcm)
        if pos + n > total:
            n = total - pos
        if n <= 0:
            continue
        for i in range(n):
            acc[pos + i] += pcm[i]
    return acc


def acc_to_s16le(acc: array.array, normalize: bool, peak_target: int = 28000) -> tuple[bytes, int]:
    peak = 1
    for x in acc:
        a = -x if x < 0 else x
        if a > peak:
            peak = a
    buf = bytearray(len(acc) * 2)
    mv = memoryview(buf).cast("h")
    if normalize and peak > 0:
        for i, x in enumerate(acc):
            v = int(x * peak_target / peak)
            if v > 32767:
                v = 32767
            elif v < -32768:
                v = -32768
            mv[i] = v
    else:
        for i, x in enumerate(acc):
            v = x
            if v > 32767:
                v = 32767
            elif v < -32768:
                v = -32768
            mv[i] = v
    mv.release()
    return bytes(buf), peak


def write_pcm16_wav(path: str, pcm: bytes, rate: int = MIX_RATE) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def _fmt_bytes(n: int) -> str:
    if n >= 1024 * 1024:
        return f"{n}  ({n / 1024 / 1024:.2f} MB)"
    if n >= 1024:
        return f"{n}  ({n / 1024:.1f} KB)"
    return str(n)


class DebugPad(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("S3 混音快捷调试")
        self.geometry("1840x1000")
        self.minsize(1280, 720)
        self._ui_font = ("Microsoft YaHei UI", 9)
        try:
            style = ttk.Style(self)
            style.configure(".", font=self._ui_font)
            style.configure("Treeview", font=self._ui_font)
            style.configure("Treeview.Heading", font=self._ui_font)
        except tk.TclError:
            self._ui_font = ("TkDefaultFont", 9)
        self.ser: serial.Serial | None = None
        self.sock: socket.socket | None = None
        self._held: set[int] = set()
        self._score_notes: list[tuple[int, int]] = []
        self._score_title = ""
        self._sample_dir = ""
        self._score_stop = threading.Event()
        self._score_pause = threading.Event()
        self._score_thread: threading.Thread | None = None
        self._tx_q: queue.Queue[bytes] = queue.Queue(maxsize=32)
        self._rx_q: queue.Queue[bytes] = queue.Queue(maxsize=1024)
        self._rx_buf = ""
        self._nk_buf = bytearray()
        self._in_status = False
        self._status_lines: list[str] = []
        self._in_scores = False
        self._in_music = False
        self._score_files: list[tuple[str, str]] = []
        self._music_files: list[tuple[str, str]] = []
        self._in_score_info = False
        self._score_info_lines: list[str] = []
        self._listing = False
        self._opening = False
        self._list_meta: dict[str, str] = {}
        self._io_run = True
        self._log_skip = 0
        self._io_thread = threading.Thread(target=self._io_loop, name="serial-io", daemon=True)
        self._io_thread.start()
        self._scope_hold_bus = 0
        self._scope_hold_out = 0
        self._scope_zoom = 1.0
        self._scope_auto = True
        self._hist_bus: list[int] = []
        self._hist_out: list[int] = []
        self._hist_lim = 360
        self._last_wave_out: tuple[int, ...] = ()
        self._last_wave_bus: tuple[int, ...] = ()
        self._build()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(80, self._pump_rx)
        self.after(1500, self._poll_status)

    def _build(self) -> None:
        top = ttk.Frame(self, padding=8)
        top.pack(fill="x")
        ttk.Label(top, text="串口").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(top, textvariable=self.port_var, width=48, state="readonly")
        self.port_box.pack(side="left", padx=6)
        ttk.Button(top, text="刷新", command=self._refresh_ports).pack(side="left")
        self.conn_btn = ttk.Button(top, text="连接", command=self._toggle_conn)
        self.conn_btn.pack(side="left", padx=6)
        ttk.Label(top, text="TCP").pack(side="left", padx=(16, 0))
        self.tcp_host = tk.StringVar(value="192.168.110.180")
        ttk.Entry(top, textvariable=self.tcp_host, width=16).pack(side="left", padx=4)
        ttk.Label(top, text=":2333").pack(side="left")
        self.tcp_btn = ttk.Button(top, text="连 TCP", command=self._toggle_tcp)
        self.tcp_btn.pack(side="left", padx=6)
        self.state_var = tk.StringVar(value="未连接（调试用 TCP，COM 只烧录/看 log）")
        ttk.Label(top, textvariable=self.state_var).pack(side="left", padx=12)
        self._refresh_ports()

        wrap = ttk.Frame(self)
        wrap.pack(fill="both", expand=True)
        self._page = tk.Canvas(wrap, highlightthickness=0)
        vsb = ttk.Scrollbar(wrap, orient="vertical", command=self._page.yview)
        hsb = ttk.Scrollbar(wrap, orient="horizontal", command=self._page.xview)
        self._page.configure(yscrollcommand=vsb.set, xscrollcommand=hsb.set)
        vsb.pack(side="right", fill="y")
        hsb.pack(side="bottom", fill="x")
        self._page.pack(side="left", fill="both", expand=True)
        body = ttk.Frame(self._page)
        self._page_win = self._page.create_window(0, 0, window=body, anchor="nw")

        def _body_cfg(_e=None) -> None:
            self._page.configure(scrollregion=self._page.bbox("all"))

        def _page_cfg(e) -> None:
            self._page.itemconfigure(self._page_win, width=max(e.width, body.winfo_reqwidth()))

        body.bind("<Configure>", _body_cfg)
        self._page.bind("<Configure>", _page_cfg)

        def _wheel_page(e) -> None:
            w = getattr(e, "widget", None)
            if getattr(self, "_scope_widgets", None) and w in self._scope_widgets:
                return
            try:
                if w is not None and w.winfo_class() in ("Treeview", "Text"):
                    return
            except tk.TclError:
                pass
            self._page.yview_scroll(int(-e.delta / 120), "units")

        self._page.bind_all("<MouseWheel>", _wheel_page)

        left = ttk.Frame(body)
        left.pack(side="left", fill="both", expand=True)

        sysf = ttk.LabelFrame(left, text="系统资源（TCP 约 10Hz 推送）", padding=8)
        sysf.pack(fill="x", padx=8, pady=4)
        self.sys_vars = {
            "rate": tk.StringVar(value="-"),
            "sd_ok": tk.StringVar(value="-"),
            "sd_err": tk.StringVar(value="-"),
            "sd_pins": tk.StringVar(value="-"),
            "bank": tk.StringVar(value="-"),
            "voices": tk.StringVar(value="-"),
            "poly": tk.StringVar(value="-"),
            "playing": tk.StringVar(value="-"),
            "master": tk.StringVar(value="-"),
            "heap": tk.StringVar(value="-"),
            "psram": tk.StringVar(value="-"),
            "samples": tk.StringVar(value="-"),
            "usb": tk.StringVar(value="-"),
            "usb_mounted": tk.StringVar(value="-"),
            "tone": tk.StringVar(value="-"),
            "enc": tk.StringVar(value="-"),
            "dev_score": tk.StringVar(value="-"),
            "hpf": tk.StringVar(value="-"),
            "wifi": tk.StringVar(value="-"),
            "ble": tk.StringVar(value="-"),
        }
        rows = [
            ("采样率", "rate"),
            ("SD 卡", "sd_ok"),
            ("SD 错误", "sd_err"),
            ("SD 接线", "sd_pins"),
            ("音色库", "bank"),
            ("当前音色", "tone"),
            ("USB 身份", "usb"),
            ("USB 枚举", "usb_mounted"),
            ("编码器", "enc"),
            ("板内谱子", "dev_score"),
            ("高通/限幅", "hpf"),
            ("WiFi", "wifi"),
            ("蓝牙", "ble"),
            ("活跃声部", "voices"),
            ("复音上限", "poly"),
            ("正在播放键", "playing"),
            ("主音量", "master"),
            ("内部 SRAM", "heap"),
            ("PSRAM", "psram"),
            ("已输出采样", "samples"),
        ]
        for i, (label, key) in enumerate(rows):
            ttk.Label(sysf, text=label + "：", width=12).grid(row=i, column=0, sticky="w")
            ttk.Label(sysf, textvariable=self.sys_vars[key]).grid(row=i, column=1, sticky="w")

        bankf = ttk.LabelFrame(left, text="钢琴采样 1–15（加载 / 时长 / 峰值）", padding=8)
        bankf.pack(fill="both", expand=True, padx=8, pady=4)
        cols = ("键", "状态", "时长", "峰值", "路径")
        self.tree = ttk.Treeview(bankf, columns=cols, show="headings", height=8)
        for c, w in zip(cols, (40, 70, 70, 70, 280)):
            self.tree.heading(c, text=c)
            self.tree.column(c, width=w, anchor="w")
        self.tree.pack(fill="both", expand=True)
        for i in range(1, 16):
            self.tree.insert("", "end", iid=str(i), values=(i, "未知", "-", "-", "-"))

        sdf = ttk.LabelFrame(left, text="SD 谱子（板内读取 / 打开 / 播放）", padding=8)
        sdf.pack(fill="both", expand=True, padx=8, pady=4)
        sbtns = ttk.Frame(sdf)
        sbtns.pack(fill="x")
        ttk.Button(sbtns, text="刷新列表", command=self._refresh_sd).pack(side="left", padx=3)
        ttk.Button(sbtns, text="打开选中", command=self._sd_open_selected).pack(side="left", padx=3)
        ttk.Button(sbtns, text="播放", command=lambda: self._send("score play")).pack(side="left", padx=3)
        ttk.Button(sbtns, text="暂停", command=lambda: self._send("score pause")).pack(side="left", padx=3)
        ttk.Button(sbtns, text="停止", command=lambda: self._send("score stop")).pack(side="left", padx=3)
        self.dev_score_info = tk.StringVar(value="点「刷新列表」读取 /sdcard/puzi")
        ttk.Label(sdf, textvariable=self.dev_score_info, wraplength=480).pack(anchor="w", pady=2)
        scols = ("文件", "大小")
        self.score_tree = ttk.Treeview(sdf, columns=scols, show="headings", height=8)
        self.score_tree.heading("文件", text="文件")
        self.score_tree.heading("大小", text="大小")
        self.score_tree.column("文件", width=380, anchor="w")
        self.score_tree.column("大小", width=80, anchor="w")
        sy = ttk.Scrollbar(sdf, orient="vertical", command=self.score_tree.yview)
        self.score_tree.configure(yscrollcommand=sy.set)
        self.score_tree.pack(side="left", fill="both", expand=True)
        sy.pack(side="right", fill="y")
        self.score_tree.bind("<Double-1>", lambda e: self._sd_open_selected())

        mf = ttk.LabelFrame(left, text="SD 音乐 /sdcard/music（外接功放试听，PCM WAV）", padding=8)
        mf.pack(fill="both", expand=True, padx=8, pady=4)
        mbtns = ttk.Frame(mf)
        mbtns.pack(fill="x")
        ttk.Button(mbtns, text="刷新", command=self._refresh_music).pack(side="left", padx=3)
        ttk.Button(mbtns, text="播放选中", command=self._music_play_sel).pack(side="left", padx=3)
        ttk.Button(mbtns, text="停止", command=lambda: self._send("music stop")).pack(side="left", padx=3)
        self.music_info = tk.StringVar(value="把 16bit PCM WAV 放到 TF 的 music 文件夹")
        ttk.Label(mf, textvariable=self.music_info, wraplength=480).pack(anchor="w", pady=2)
        self.music_tree = ttk.Treeview(mf, columns=("文件",), show="headings", height=5)
        self.music_tree.heading("文件", text="文件")
        self.music_tree.column("文件", width=440, anchor="w")
        self.music_tree.pack(fill="both", expand=True)
        self.music_tree.bind("<Double-1>", lambda e: self._music_play_sel())

        right = ttk.Frame(body)
        right.pack(side="left", fill="both", expand=True)

        keys = ttk.LabelFrame(right, text="钢琴采样：点一下播完，连点可叠同一音", padding=8)
        keys.pack(fill="x", padx=8, pady=4)
        for r in range(3):
            row = ttk.Frame(keys)
            row.pack()
            for c in range(5):
                n = r * 5 + c + 1
                b = tk.Button(row, text=str(n), width=6, height=2)
                b.pack(side="left", padx=3, pady=3)
                b.bind("<ButtonPress-1>", lambda e, k=n: self._note_down(k))

        acts = ttk.Frame(right, padding=8)
        acts.pack(fill="x")
        ttk.Button(acts, text="四音和弦", command=lambda: self._send("chord 1 5 8 12")).pack(side="left", padx=3)
        ttk.Button(acts, text="八音", command=lambda: self._send("chord 1 2 3 4 5 6 7 8")).pack(side="left", padx=3)
        ttk.Button(acts, text="15 音全开", command=lambda: self._send("chord 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15")).pack(side="left", padx=3)
        ttk.Button(acts, text="停止", command=lambda: self._send("stop")).pack(side="left", padx=3)

        ident = ttk.LabelFrame(right, text="USB 身份 / 音色（BOOT 键也可循环 USB）", padding=8)
        ident.pack(fill="x", padx=8, pady=4)
        irow = ttk.Frame(ident)
        irow.pack(fill="x")
        ttk.Button(irow, text="USB HID", command=lambda: self._send("usb hid")).pack(side="left", padx=3)
        ttk.Button(irow, text="USB MIDI", command=lambda: self._send("usb midi")).pack(side="left", padx=3)
        ttk.Button(irow, text="USB 关", command=lambda: self._send("usb off")).pack(side="left", padx=3)
        ttk.Button(irow, text="循环身份", command=lambda: self._send("usb")).pack(side="left", padx=3)
        irow2 = ttk.Frame(ident)
        irow2.pack(fill="x", pady=4)
        ttk.Button(irow2, text="钢琴库", command=lambda: self._send("bank piano")).pack(side="left", padx=3)
        ttk.Button(irow2, text="小提琴库", command=lambda: self._send("bank violin")).pack(side="left", padx=3)
        ttk.Button(irow2, text="模拟 HID/MIDI 键6", command=lambda: self._send("usb tap 6")).pack(side="left", padx=3)
        ttk.Label(ident, text="HID 用原生 USB（VID 303A PID 40A1）。下载/本窗口仍用 CH343 COM18。切模式不会重新枚举。").pack(anchor="w")

        hidf = ttk.LabelFrame(right, text="键盘 / MIDI 捕获（窗口有焦点时拦截按键，避免打到记事本）", padding=8)
        hidf.pack(fill="x", padx=8, pady=4)
        ttk.Label(hidf, text="点本窗口任意处后再按物理键。上方 EVENT 来自板子；下面 host 来自电脑收到的键。").pack(anchor="w")
        self.hid_log = tk.Text(hidf, height=6, font=("Consolas", 9), bg="#1e1e1e", fg="#dcdcdc")
        self.hid_log.pack(fill="x")
        self.bind_all("<KeyPress>", self._on_host_key)

        vol = ttk.Frame(right, padding=8)
        vol.pack(fill="x")
        ttk.Label(vol, text="主音量").pack(side="left")
        self.vol = tk.Scale(vol, from_=0, to=255, orient="horizontal", length=280)
        self.vol.set(8)
        self.vol.pack(side="left", padx=8)
        self.vol.bind("<ButtonRelease-1>", lambda e: self._send(f"master {int(self.vol.get())}"))

        dsp = ttk.Frame(right, padding=4)
        dsp.pack(fill="x")
        ttk.Button(dsp, text="高通 90Hz", command=lambda: self._send("hpf 90")).pack(side="left", padx=3)
        ttk.Button(dsp, text="高通关", command=lambda: self._send("hpf 0")).pack(side="left", padx=3)
        ttk.Button(dsp, text="限幅 28000", command=lambda: self._send("limit 28000")).pack(side="left", padx=3)
        ttk.Button(dsp, text="限幅关", command=lambda: self._send("limit 0")).pack(side="left", padx=3)
        ttk.Button(dsp, text="衰减 0dB", command=lambda: self._send("pad 0")).pack(side="left", padx=3)
        ttk.Button(dsp, text="衰减 18dB", command=lambda: self._send("pad 18")).pack(side="left", padx=3)
        ttk.Button(dsp, text="衰减 24dB", command=lambda: self._send("pad 24")).pack(side="left", padx=3)

        netf = ttk.LabelFrame(right, text="WiFi / BLE 单元测试（网线可继续上网，另连 S3Keys）", padding=8)
        netf.pack(fill="x", padx=8, pady=4)
        nrow = ttk.Frame(netf)
        nrow.pack(fill="x")
        ttk.Button(nrow, text="开 AP", command=lambda: self._send("wifi ap")).pack(side="left", padx=3)
        ttk.Button(nrow, text="关 WiFi", command=lambda: self._send("wifi off")).pack(side="left", padx=3)
        ttk.Button(nrow, text="TCP 试 play 1", command=self._tcp_play1).pack(side="left", padx=3)
        ttk.Button(nrow, text="BLE 广播", command=lambda: self._send("ble on")).pack(side="left", padx=3)
        ttk.Button(nrow, text="关 BLE", command=lambda: self._send("ble off")).pack(side="left", padx=3)
        ttk.Label(netf, text="WiFi：S3Keys / 12345678 → 192.168.110.180:2333。BLE：nRF Connect 扫 S3Keys（可连接广播下一阶段才做）。开射频时屏会降帧，避免花屏。").pack(anchor="w")

        sine = ttk.LabelFrame(right, text="正弦对照（不走 SD）", padding=8)
        sine.pack(fill="x", padx=8)
        for i in (0, 4, 7, 9, 12):
            ttk.Button(sine, text=f"sine {i}", command=lambda n=i: self._send(f"sine {n}")).pack(side="left", padx=3)

        scoref = ttk.LabelFrame(right, text="谱子演奏（打开 JSON / txt，1Key+2Key 合并）", padding=8)
        scoref.pack(fill="x", padx=8, pady=4)
        srow = ttk.Frame(scoref)
        srow.pack(fill="x")
        ttk.Button(srow, text="打开谱子", command=self._open_score).pack(side="left", padx=3)
        ttk.Button(srow, text="播放", command=self._score_play).pack(side="left", padx=3)
        ttk.Button(srow, text="暂停/继续", command=self._score_toggle_pause).pack(side="left", padx=3)
        ttk.Button(srow, text="停止谱子", command=self._score_stop_play).pack(side="left", padx=3)
        ttk.Button(srow, text="导出 WAV", command=self._export_score_wav).pack(side="left", padx=3)
        self.score_info = tk.StringVar(value="未加载谱子")
        ttk.Label(scoref, textvariable=self.score_info, wraplength=420).pack(anchor="w", pady=4)

        logf = ttk.LabelFrame(right, text="串口日志", padding=4)
        logf.pack(fill="both", expand=True, padx=8, pady=4)
        self.log = tk.Text(logf, height=8, font=("Consolas", 9))
        self.log.pack(fill="both", expand=True)
        self._log("本工程控制台在 UART0：请选 CH343 口（现在是 COM18），不要选 USB 串行设备 COM20。")
        self._log("COM20 是板载 USB-JTAG，固件没开这个控制台，打开还可能被 DTR 复位卡死。")

        self._build_scope_col(body)

    def _build_scope_col(self, parent: ttk.Frame) -> None:
        col = ttk.Frame(parent)
        col.pack(side="left", fill="both", expand=True)
        head = ttk.LabelFrame(col, text="示波器列（自动放大小信号；滚轮缩放，中键复位）", padding=6)
        head.pack(fill="x", padx=6, pady=4)
        self.scope_info = tk.StringVar(value="连 TCP 后这里会动。以前几乎不动是因为按 ±32768 画，音量 16 时输出只有几十。")
        ttk.Label(head, textvariable=self.scope_info, wraplength=520).pack(anchor="w")
        btns = ttk.Frame(head)
        btns.pack(fill="x", pady=4)
        ttk.Button(btns, text="自动放大", command=self._scope_reset_zoom).pack(side="left", padx=3)
        ttk.Button(btns, text="固定 ±32768", command=self._scope_fullscale).pack(side="left", padx=3)
        ttk.Label(btns, text="在波形上滚轮放大/缩小").pack(side="left", padx=8)
        meter = ttk.Frame(head)
        meter.pack(fill="x", pady=2)
        ttk.Label(meter, text="总线").pack(side="left")
        self.scope_bus_c = tk.Canvas(meter, width=220, height=18, bg="#222", highlightthickness=0)
        self.scope_bus_c.pack(side="left", padx=4, fill="x", expand=True)
        ttk.Label(meter, text="输出").pack(side="left", padx=(8, 0))
        self.scope_out_c = tk.Canvas(meter, width=220, height=18, bg="#222", highlightthickness=0)
        self.scope_out_c.pack(side="left", padx=4, fill="x", expand=True)

        self.scope_bus_wave = self._mk_scope(col, "总线波形（叠音看这个，自动缩放）", 220)
        self.scope_out_wave = self._mk_scope(col, "I2S 输出波形（×主音量之后）", 180)
        self.scope_hist = self._mk_scope(col, "峰值历史（横轴≈时间，谱子连弹看起伏）", 160)

    def _mk_scope(self, parent: ttk.Frame, title: str, height: int) -> tk.Canvas:
        box = ttk.LabelFrame(parent, text=title, padding=4)
        box.pack(fill="both", expand=True, padx=6, pady=4)
        cv = tk.Canvas(box, height=height, bg="#101418", highlightthickness=0)
        cv.pack(fill="both", expand=True)
        cv.bind("<MouseWheel>", self._on_scope_wheel)
        cv.bind("<Button-2>", lambda e: self._scope_reset_zoom())
        cv.bind("<Double-Button-1>", lambda e: self._scope_fullscale())
        if not hasattr(self, "_scope_widgets"):
            self._scope_widgets = set()
        self._scope_widgets.add(cv)
        return cv

    def _scope_reset_zoom(self) -> None:
        self._scope_auto = True
        self._scope_zoom = 1.0
        self._redraw_scopes()

    def _scope_fullscale(self) -> None:
        self._scope_auto = False
        self._scope_zoom = 1.0
        self._redraw_scopes()

    def _on_scope_wheel(self, e) -> str:
        steps = e.delta / 120.0
        self._scope_zoom = min(32.0, max(0.25, self._scope_zoom * (1.18 ** steps)))
        self._scope_auto = True
        self._redraw_scopes()
        return "break"

    def _port_choices(self) -> list[str]:
        items = []
        for p in list_ports.comports():
            extra = p.description or ""
            if "303A" in (p.hwid or "") or "USB 串行设备" in extra:
                extra += "  [USB-JTAG，不要用]"
            elif "CH343" in extra or "CH340" in extra or "CP210" in extra:
                extra += "  [UART0，用这个]"
            items.append(f"{p.device}  |  {extra}")
        return items

    def _selected_com(self) -> str:
        raw = self.port_var.get()
        return raw.split(" ", 1)[0].strip() if raw else ""

    def _refresh_ports(self) -> None:
        items = self._port_choices()
        self.port_box["values"] = items
        preferred = next((i for i in items if "UART0" in i), items[0] if items else "")
        cur = self.port_var.get()
        if preferred and (not cur or cur not in items or "USB-JTAG" in cur):
            self.port_var.set(preferred)

    def _toggle_conn(self) -> None:
        if self.ser is not None:
            self._disconnect()
            return
        port = self._selected_com()
        if not port:
            messagebox.showerror("串口", "没有可用 COM 口")
            return
        if port.upper() == "COM20":
            if not messagebox.askyesno(
                "口选错了？",
                "COM20 是 ESP32-S3 的 USB-JTAG，当前固件日志和命令在 CH343 的 COM18。\n"
                "强行打开 COM20 容易卡死。仍要打开吗？",
            ):
                return
        self.conn_btn.config(state="disabled")
        self.state_var.set(f"正在打开 {port} …")
        threading.Thread(target=self._open_port, args=(port,), daemon=True).start()

    def _open_port(self, port: str) -> None:
        err = None
        ser = None
        try:
            ser = serial.Serial()
            ser.port = _win_port(port)
            ser.baudrate = BAUD
            ser.timeout = 0.03
            ser.write_timeout = 0.5
            ser.dsrdtr = False
            ser.rtscts = False
            ser.open()
            try:
                ser.setDTR(False)
                ser.setRTS(False)
            except Exception:
                pass
            try:
                ser.reset_input_buffer()
            except Exception:
                pass
        except Exception as exc:  # noqa: BLE001
            err = exc
            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
        self.after(0, lambda: self._finish_open(port, ser, err))

    def _finish_open(self, port: str, ser: serial.Serial | None, err: Exception | None) -> None:
        self.conn_btn.config(state="normal")
        if err or ser is None:
            self.state_var.set("未连接")
            messagebox.showerror("串口", f"打开 {port} 失败:\n{err}")
            return
        self.ser = ser
        self.conn_btn.config(text="断开")
        self.state_var.set(f"已连接 {port}")
        self._log(f"已连接 {port} {BAUD}（未拉 DTR/RTS，避免复位）")
        self.after(400, lambda: self._send("status", silent=True))

    def _disconnect(self) -> None:
        ser = self.ser
        self.ser = None
        sock = self.sock
        self.sock = None
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
        self.conn_btn.config(text="连接")
        self.tcp_btn.config(text="连 TCP")
        self.state_var.set("未连接")
        self._log("已断开")

    def _toggle_tcp(self) -> None:
        if self.sock is not None:
            self._disconnect()
            return
        host = (self.tcp_host.get() or "").strip()
        if not host:
            messagebox.showerror("TCP", "填写板子 IP（屏幕上 STA/AP 后面那串）")
            return
        self.tcp_btn.config(state="disabled")

        def go() -> None:
            err = None
            sock = None
            try:
                sock = socket.create_connection((host, 2333), timeout=5)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.settimeout(0.05)
            except Exception as exc:  # noqa: BLE001
                err = exc
                if sock is not None:
                    try:
                        sock.close()
                    except Exception:
                        pass
                    sock = None
            self.after(0, lambda: self._finish_tcp(host, sock, err))

        threading.Thread(target=go, daemon=True).start()

    def _finish_tcp(self, host: str, sock: socket.socket | None, err: Exception | None) -> None:
        self.tcp_btn.config(state="normal")
        if err or sock is None:
            self.state_var.set("未连接")
            messagebox.showerror("TCP", f"连 {host}:2333 失败:\n{err}")
            return
        self.sock = sock
        self.tcp_btn.config(text="断开 TCP")
        self.state_var.set(f"TCP {host}:2333")
        self._log(f"已连 TCP {host}:2333（状态由板子推送，COM18 只看 log/烧录）")
        self.after(300, lambda: self._send("status"))

    def _apply_nk_status(self, raw: bytes) -> None:
        if len(raw) < NK_STATUS_STRUCT.size:
            return
        t = NK_STATUS_STRUCT.unpack_from(raw)
        flags = t[1]
        slot_mask = t[10]
        bank = t[14].split(b"\0", 1)[0].decode("utf-8", "replace")
        ip = t[15].split(b"\0", 1)[0].decode("utf-8", "replace")
        tcp_n = t[16] if len(t) > 16 else 0
        nload = bin(slot_mask).count("1")
        kv = {
            "rate": "22050",
            "block": "256",
            "sd_ok": "1" if flags & 1 else "0",
            "factory_ok": "1" if flags & 2 else "0",
            "usb_mounted": "1" if flags & 4 else "0",
            "voices": str(t[2]),
            "poly": str(t[3]),
            "master": str(t[4]),
            "hpf": str(t[5]),
            "limit": str(t[6]),
            "score_notes": str(t[7]),
            "score_idx": str(t[8]),
            "voice_mask": str(t[9]),
            "heap_int_free": str(t[11]),
            "psram_free": str(t[12]),
            "samples": str(t[13]),
            "tone": bank,
            "bank": f"{nload}/15",
            "wifi_ip": ip,
            "wifi": "sta" if ip and not ip.startswith("192.168.4.") else "ap",
            "wifi_tcp": "2333",
            "wifi_clients": str(tcp_n),
            "score_play": "1" if flags & 8 else "0",
            "score_pause": "1" if flags & 16 else "0",
        }
        lines = [f"{k}={v}" for k, v in kv.items()]
        lines.append(f"dev_score idx={kv['score_idx']}/{kv['score_notes']}")
        self._apply_status(lines)
        for i in range(1, 16):
            iid = str(i)
            if not self.tree.exists(iid):
                continue
            cur = self.tree.item(iid, "values")
            st = str(cur[1]) if cur and len(cur) > 1 else ""
            if st not in ("未知", "", "-"):
                continue
            loaded = bool(slot_mask & (1 << (i - 1)))
            self.tree.item(
                iid,
                values=(i, "已加载" if loaded else "缺失", "-", "-", "-"),
            )

    def _scope_meter(self, canvas: tk.Canvas, value: int, full: int, hold: int, hot: bool) -> None:
        canvas.delete("all")
        w = max(int(canvas.winfo_width()), 8)
        h = max(int(canvas.winfo_height()), 8)
        canvas.create_rectangle(0, 0, w, h, fill="#1a1a1a", outline="")
        def px(v: int) -> int:
            if full <= 0:
                return 0
            return max(0, min(w, int(v * w / full)))
        canvas.create_rectangle(0, 0, px(value), h, fill="#c44" if hot else "#3a8", outline="")
        xh = px(hold)
        canvas.create_line(xh, 0, xh, h, fill="#eee")
        mark = px(32767) if full > 32767 else w
        canvas.create_line(mark, 0, mark, h, fill="#fc6")

    def _draw_wave(self, cv: tk.Canvas, wave: tuple[int, ...], scale: int, color: str, tag: str) -> None:
        cv.delete("all")
        w = max(int(cv.winfo_width()), 40)
        h = max(int(cv.winfo_height()), 40)
        mid = h // 2
        cv.create_rectangle(0, 0, w, h, fill="#101418", outline="")
        cv.create_line(0, mid, w, mid, fill="#333")
        if scale < 8:
            scale = 8
        n = len(wave)
        if n < 2:
            return
        pts = []
        for i, s in enumerate(wave):
            x = int(i * (w - 1) / (n - 1))
            y = mid - int(s * (mid - 8) * self._scope_zoom / scale)
            y = max(1, min(h - 2, y))
            pts.extend((x, y))
        cv.create_line(*pts, fill=color, smooth=True)
        cv.create_text(8, 12, anchor="w", fill="#aaa", font=("Consolas", 9),
                       text=f"{tag}  ±{scale}  x{self._scope_zoom:.2f}")

    def _draw_hist(self, cv: tk.Canvas, hist: list[int], color: str, tag: str, fs_mark: int) -> None:
        cv.delete("all")
        w = max(int(cv.winfo_width()), 40)
        h = max(int(cv.winfo_height()), 40)
        cv.create_rectangle(0, 0, w, h, fill="#101418", outline="")
        if not hist:
            return
        peak = max(hist) if hist else 1
        if self._scope_auto:
            scale = max(64, peak)
        else:
            scale = max(fs_mark, 1)
        n = len(hist)
        pts = [0, h]
        for i, v in enumerate(hist):
            x = int(i * (w - 1) / max(n - 1, 1))
            y = h - 4 - int(v * (h - 10) / scale)
            y = max(2, min(h - 2, y))
            pts.extend((x, y))
        pts.extend((w, h))
        cv.create_polygon(*pts, fill="#1c2a22", outline=color)
        if scale > fs_mark:
            y327 = h - 4 - int(fs_mark * (h - 10) / scale)
            cv.create_line(0, y327, w, y327, fill="#fc6", dash=(4, 3))
        cv.create_text(8, 12, anchor="w", fill="#aaa", font=("Consolas", 9),
                       text=f"{tag} hold {peak}")

    def _redraw_scopes(self) -> None:
        if not hasattr(self, "scope_bus_wave"):
            return
        wave_out = self._last_wave_out
        wave_bus = self._last_wave_bus
        if self._scope_auto:
            bus_sc = max(64, max((abs(x) for x in wave_bus), default=64), self._scope_hold_bus // 4)
            out_sc = max(32, max((abs(x) for x in wave_out), default=32))
        else:
            bus_sc = 32768
            out_sc = 32768
        self._draw_wave(self.scope_bus_wave, wave_bus, bus_sc, "#6ecbff", "bus")
        self._draw_wave(self.scope_out_wave, wave_out, out_sc, "#6fdc8c", "out")
        self._draw_hist(self.scope_hist, self._hist_bus, "#6ecbff", "总线峰", 32767)

    def _apply_nk_scope(self, raw: bytes) -> None:
        if len(raw) >= NK_SCOPE_STRUCT.size:
            t = NK_SCOPE_STRUCT.unpack_from(raw)
            self._last_wave_out = t[10:74]
            self._last_wave_bus = t[74:138]
        elif len(raw) >= NK_SCOPE_STRUCT.size - 128:
            t = struct.Struct("<iiiiIHHBBH64h").unpack_from(raw)
            self._last_wave_out = t[10:74]
            self._last_wave_bus = self._last_wave_out
        else:
            return
        peak_bus, peak_hpf, peak_lim, peak_out, mean_abs, clip_bus, sat_out, voices, limited, master = t[:10]
        self._scope_hold_bus = max(peak_bus, int(self._scope_hold_bus * 0.94))
        self._scope_hold_out = max(peak_out, int(self._scope_hold_out * 0.94))
        self._hist_bus.append(peak_bus)
        self._hist_out.append(peak_out)
        if len(self._hist_bus) > self._hist_lim:
            self._hist_bus = self._hist_bus[-self._hist_lim :]
            self._hist_out = self._hist_out[-self._hist_lim :]
        bus_hot = peak_bus > 32767 or clip_bus > 0
        out_hot = peak_out > 32767 or sat_out > 0
        self._scope_meter(self.scope_bus_c, peak_bus, max(65536, self._scope_hold_bus, peak_bus), self._scope_hold_bus, bus_hot)
        self._scope_meter(self.scope_out_c, peak_out, max(32768, self._scope_hold_out, peak_out), self._scope_hold_out, out_hot)
        lim_s = "限幅中" if limited else "未限幅"
        clip_s = f"总线超满幅 {clip_bus} 点" if clip_bus else "总线未超 32767"
        sat_s = f"输出 sat {sat_out}" if sat_out else "输出未 sat16"
        self.scope_info.set(
            f"声部 {voices}  master {master}/255  {lim_s}  缩放 x{self._scope_zoom:.2f}\n"
            f"总线峰 {peak_bus}  均幅 {mean_abs}  HPF {peak_hpf}  限幅后 {peak_lim}  输出峰 {peak_out}\n"
            f"{clip_s}；{sat_s}。黄线=32767。波形默认按当前峰值撑满，不是按满幅。"
        )
        self._redraw_scopes()

    def _feed_nk(self, chunk: bytes) -> None:
        self._nk_buf.extend(chunk)
        buf = self._nk_buf
        while True:
            if not buf:
                break
            if buf[0] != NK_MAGIC:
                try:
                    nli = bytes(buf).find(b"\n")
                except Exception:
                    nli = -1
                if nli >= 0:
                    line = bytes(buf[:nli]).decode("utf-8", "replace")
                    del buf[: nli + 1]
                    self.after(0, lambda s=line: self._handle_line(s.rstrip("\r")))
                    continue
                if buf[0] != NK_MAGIC:
                    del buf[0]
                    continue
            if len(buf) < 3:
                break
            body = buf[1] | (buf[2] << 8)
            if body < 1 or body > 2048:
                del buf[0]
                continue
            need = 3 + body + 1
            if len(buf) < need:
                break
            payload = bytes(buf[3 : 3 + body])
            x = buf[3 + body]
            del buf[:need]
            xor = 0
            for b in payload:
                xor ^= b
            if xor != x or not payload:
                continue
            typ, data = payload[0], payload[1:]
            if typ == NK_STATUS:
                self.after(0, lambda d=data: self._apply_nk_status(d))
            elif typ == NK_SCOPE:
                self.after(0, lambda d=data: self._apply_nk_scope(d))
            elif typ == NK_TEXT and data:
                try:
                    self._rx_q.put_nowait(data)
                except queue.Full:
                    try:
                        self._rx_q.get_nowait()
                    except queue.Empty:
                        pass
                    try:
                        self._rx_q.put_nowait(data)
                    except queue.Full:
                        pass


    def _io_loop(self) -> None:
        while self._io_run:
            sock = self.sock
            ser = self.ser
            if sock is None and (ser is None or not getattr(ser, "is_open", False)):
                time.sleep(0.03)
                continue
            try:
                if sock is not None:
                    try:
                        chunk = sock.recv(4096)
                    except TimeoutError:
                        chunk = b""
                    except OSError:
                        chunk = b""
                    if chunk:
                        self._feed_nk(chunk)
                    try:
                        for _ in range(8):
                            payload = self._tx_q.get_nowait()
                            sock.sendall(payload)
                    except queue.Empty:
                        pass
                    continue
                chunk = ser.read(4096)
                if chunk:
                    try:
                        self._rx_q.put_nowait(chunk)
                    except queue.Full:
                        try:
                            self._rx_q.get_nowait()
                        except queue.Empty:
                            pass
                try:
                    for _ in range(8):
                        payload = self._tx_q.get_nowait()
                        ser.write(payload)
                except queue.Empty:
                    pass
                if not chunk:
                    time.sleep(0.01)
            except Exception:
                time.sleep(0.05)

    def _send(self, line: str, silent: bool = False) -> None:
        if self.ser is None and self.sock is None:
            if not silent:
                self._log("未连接: " + line)
            return
        if silent and self._tx_q.qsize() > 0:
            return
        payload = nk_cmd_frame(line) if self.sock is not None else (line + "\n").encode("utf-8")
        try:
            self._tx_q.put_nowait(payload)
        except queue.Full:
            try:
                self._tx_q.get_nowait()
            except queue.Empty:
                pass
            try:
                self._tx_q.put_nowait(payload)
            except queue.Full:
                if not silent:
                    self._log("发送队列满（已丢弃最旧命令）")
                return
        if not silent:
            self._log("> " + line)

    def _note_down(self, n: int) -> None:
        self._send(f"play {n}")

    def _poll_status(self) -> None:
        # list/open 已在固件后台执行，这里继续轮询；卡住的协议状态要清掉。
        if self._in_status and len(self._status_lines) > 0:
            self._in_status = False
            self._apply_status(self._status_lines)
        if self._in_score_info:
            self._in_score_info = False
            self._opening = False
        if self.ser is not None and self.sock is None and not self._listing and not self._in_status:
            self._send("status", silent=True)
        self.after(2500, self._poll_status)

    def _refresh_sd(self) -> None:
        if self._listing:
            self._log("谱子列表仍在传输，等 END_SCORES 再刷新")
            return
        self._listing = True
        self._score_files = []
        self._list_meta = {}
        self._send("score list 0 80")

    def _refresh_music(self) -> None:
        self._music_files = []
        self._send("music list")

    def _music_play_sel(self) -> None:
        sel = self.music_tree.selection()
        if not sel:
            messagebox.showinfo("音乐", "先刷新并选中一行")
            return
        path = self.music_tree.item(sel[0], "values")[0]
        self._send("music play " + path)

    def _apply_music(self, files: list[tuple[str, str]]) -> None:
        for iid in self.music_tree.get_children():
            self.music_tree.delete(iid)
        for i, (path, _sz) in enumerate(files):
            self.music_tree.insert("", "end", iid=str(i), values=(path,))
        self.music_info.set(f"/sdcard/music  {len(files)} 个 wav（双击播放）")

    def _pct(self, s: str) -> str:
        return unquote(s or "", encoding="utf-8", errors="replace")

    def _open_score(self) -> None:
        path = filedialog.askopenfilename(
            title="打开谱子",
            initialdir=r"f:\vsproject\esp32",
            filetypes=[("谱子 JSON/TXT", "*.txt *.json"), ("全部", "*.*")],
        )
        if not path:
            return
        try:
            title, notes = parse_sky_score(path)
        except Exception as exc:
            messagebox.showerror("谱子", str(exc))
            return
        self._score_stop_play()
        self._score_title = title
        self._score_notes = notes
        last = notes[-1][0] if notes else 0
        self.score_info.set(f"{title}  |  {len(notes)} 个音  |  {last/1000:.1f}s")
        self._log(f"谱子已加载: {title} notes={len(notes)}")

    def _export_score_wav(self) -> None:
        if not self._score_notes:
            messagebox.showinfo("导出", "请先打开谱子")
            return
        try:
            _ffmpeg()
        except FileNotFoundError as exc:
            messagebox.showerror("ffmpeg", str(exc))
            return
        folder = filedialog.askdirectory(
            title="选择 1.wav…15.wav 所在目录（与板上钢琴库相同）",
            initialdir=self._sample_dir or r"f:\vsproject\esp32",
        )
        if not folder:
            return
        self._sample_dir = folder
        missing = [i for i in range(1, 16) if not os.path.isfile(os.path.join(folder, f"{i}.wav"))]
        if missing:
            messagebox.showerror("采样", f"缺少 {folder}\\{missing[0]}.wav …（需要 1–15）")
            return
        safe = re.sub(r'[\\/:*?"<>|]', "_", self._score_title or "score")[:40]
        out = filedialog.asksaveasfilename(
            title="保存合成 WAV（拷到 TF /music 后 music play）",
            initialfile=f"{safe}_mix.wav",
            defaultextension=".wav",
            filetypes=[("WAV", "*.wav")],
        )
        if not out:
            return
        normalize = messagebox.askyesno(
            "导出方式",
            "是 = 峰值归一化（推荐）\n整曲当作一首歌，用 music play 听喇叭频段。\n\n"
            "否 = 满力度叠加后 sat16\n更接近板上总线直接削波的数字结果。",
        )
        notes = list(self._score_notes)
        title = self._score_title
        threading.Thread(
            target=self._export_score_worker,
            args=(folder, out, notes, title, normalize),
            daemon=True,
        ).start()

    def _export_score_worker(
        self,
        folder: str,
        out_path: str,
        notes: list[tuple[int, int]],
        title: str,
        normalize: bool,
    ) -> None:
        def log(msg: str) -> None:
            self.after(0, lambda m=msg: self._log(m))

        try:
            log(f"ffmpeg 转采样 22050/mono：{folder}")
            bank: list[array.array] = []
            for i in range(1, 16):
                p = os.path.join(folder, f"{i}.wav")
                pcm = load_key_pcm(p)
                bank.append(pcm)
                log(f"  {i}.wav  {len(pcm)} 帧  {len(pcm)/MIX_RATE:.2f}s")
            log(f"叠加 {len(notes)} 个音（one-shot，与板上相同）…")
            acc = render_score_acc(notes, bank)
            pcm, peak = acc_to_s16le(acc, normalize=normalize)
            write_pcm16_wav(out_path, pcm)
            dur = len(acc) / MIX_RATE
            mode = "归一化到 28000" if normalize else "sat16 不归一化"
            log(
                f"已写出 {out_path}\n"
                f"  {dur:.1f}s  总线峰 {peak}  {mode}  22050/mono/16bit"
            )
            self.after(
                0,
                lambda: messagebox.showinfo(
                    "导出完成",
                    f"{title}\n{out_path}\n总线峰 {peak}  {dur:.1f}s\n"
                    "拷到 TF /music，板上 music play。主音量用 30–50 对比现场弹谱。",
                ),
            )
        except Exception as exc:
            log(f"导出失败: {exc}")
            self.after(0, lambda e=exc: messagebox.showerror("导出", str(e)))

    def _score_play(self) -> None:
        if not self._score_notes:
            messagebox.showinfo("谱子", "请先打开谱子文件")
            return
        if (self.ser is None and self.sock is None):
            messagebox.showinfo("谱子", "请先连 TCP（或串口）")
            return
        if self._score_thread and self._score_thread.is_alive():
            self._score_pause.clear()
            return
        self._score_stop.clear()
        self._score_pause.clear()
        self._score_thread = threading.Thread(target=self._score_run, daemon=True)
        self._score_thread.start()

    def _score_toggle_pause(self) -> None:
        if self._score_pause.is_set():
            self._score_pause.clear()
        else:
            self._score_pause.set()

    def _score_stop_play(self) -> None:
        self._score_stop.set()
        self._score_pause.clear()
        self._send("stop", silent=True)

    def _score_run(self) -> None:
        notes = self._score_notes
        t0 = time.perf_counter()
        i = 0
        n = len(notes)
        while i < n and not self._score_stop.is_set():
            while self._score_pause.is_set() and not self._score_stop.is_set():
                time.sleep(0.03)
                t0 = time.perf_counter() - notes[i][0] / 1000.0
            if self._score_stop.is_set():
                break
            target = notes[i][0] / 1000.0
            now = time.perf_counter() - t0
            delay = target - now
            if delay > 0.002:
                time.sleep(min(delay, 0.05))
                continue
            same_t = notes[i][0]
            keys: list[int] = []
            while i < n and notes[i][0] == same_t:
                keys.append(notes[i][1])
                i += 1
            cmd = "chord " + " ".join(str(k) for k in keys)
            self._send(cmd, silent=True)
            self.after(0, lambda t=same_t, k=i: self.score_info.set(
                f"{self._score_title}  |  {k}/{n}  |  t={t}ms"
            ))
        if not self._score_stop.is_set():
            self.after(0, lambda: self.score_info.set(f"{self._score_title}  |  播放结束"))

    def _apply_status(self, lines: list[str]) -> None:
        kv: dict[str, str] = {}
        slots: dict[int, tuple] = {}
        for raw in lines:
            if raw.startswith("slot="):
                parts = dict(p.split("=", 1) for p in raw.split() if "=" in p)
                try:
                    idx = int(parts.get("slot", "0"))
                except ValueError:
                    continue
                loaded = parts.get("loaded") == "1"
                slots[idx] = (
                    idx,
                    "已加载" if loaded else "缺失",
                    f"{parts.get('sec', '-')}s" if loaded else "-",
                    parts.get("peak", "-") if loaded else "-",
                    parts.get("path", "-"),
                )
                continue
            if "=" in raw:
                k, v = raw.split("=", 1)
                kv[k] = v
        try:
            if "rate" in kv:
                self.sys_vars["rate"].set(f"{kv['rate']} Hz  block={kv.get('block', '?')}")
            if "sd_ok" in kv:
                self.sys_vars["sd_ok"].set("正常" if kv["sd_ok"] == "1" else "失败")
            if "sd_err" in kv:
                self.sys_vars["sd_err"].set(kv["sd_err"])
            if "sd_pins" in kv:
                self.sys_vars["sd_pins"].set(kv["sd_pins"])
            if "bank" in kv:
                src = ""
                if kv.get("factory_ok") == "1":
                    src = "  出厂Flash可用"
                if kv.get("sd_ok") == "1":
                    src += "  TF可用"
                self.sys_vars["bank"].set(kv["bank"] + "  （15 个都应已加载）" + src)
            if "voices" in kv:
                self.sys_vars["voices"].set(kv["voices"] + " 路正在响")
            if "poly" in kv:
                self.sys_vars["poly"].set(kv["poly"] + "  （串口 poly N 可改，上限 32）")
            if "usb" in kv:
                self.sys_vars["usb"].set(kv["usb"] + "  （BOOT / 编码器短按 / usb hid|midi|off）")
            if "usb_mounted" in kv:
                self.sys_vars["usb_mounted"].set("已枚举" if kv["usb_mounted"] == "1" else "未挂上（请插 S3 原生 USB）")
            if "enc_lvl" in kv:
                self.sys_vars["enc"].set(f"{kv.get('enc_pins', '')}  电平 {kv['enc_lvl']}  音量 {kv.get('master', '?')}")
            if "score_path" in kv:
                title = self._pct(kv.get("score_title", "-"))
                self.sys_vars["dev_score"].set(
                    f"{title}  {kv.get('score_idx', '0')}/{kv.get('score_notes', '0')}  "
                    f"play={kv.get('score_play', '0')} pause={kv.get('score_pause', '0')}"
                )
            if "hpf" in kv:
                self.sys_vars["hpf"].set(f"{kv['hpf']} Hz  limit={kv.get('limit', '?')}")
            if "wifi" in kv:
                self.sys_vars["wifi"].set(
                    f"{kv['wifi']}  {kv.get('wifi_ip', '')}:{kv.get('wifi_tcp', '')}  "
                    f"tcp客户端={kv.get('wifi_clients', '0')}"
                )
            if "ble" in kv:
                self.sys_vars["ble"].set(kv["ble"] + "  （ble on 后用 nRF Connect 扫 S3Keys）")
            if "tone" in kv:
                extra = "  占位EQ开（violin 文件和钢琴相同）" if kv.get("color") == "1" else ""
                self.sys_vars["tone"].set(kv["tone"] + "  （bank piano|violin）" + extra)
            if "voice_mask" in kv:
                mask = int(kv["voice_mask"])
                keys = [str(i + 1) for i in range(15) if mask & (1 << i)]
                self.sys_vars["playing"].set(", ".join(keys) if keys else "无")
            if "master" in kv:
                self.sys_vars["master"].set(kv["master"] + " / 255")
            if "heap_int_free" in kv:
                free = _fmt_bytes(int(kv["heap_int_free"]))
                if "heap_int_min" in kv:
                    self.sys_vars["heap"].set(
                        f"空闲 {free}  "
                        f"历史最低 {_fmt_bytes(int(kv['heap_int_min']))}  "
                        f"最大块 {_fmt_bytes(int(kv.get('heap_int_largest', '0')))}"
                    )
                else:
                    self.sys_vars["heap"].set(f"空闲 {free}")
            if "psram_free" in kv:
                free = _fmt_bytes(int(kv["psram_free"]))
                if "psram_min" in kv:
                    self.sys_vars["psram"].set(
                        f"空闲 {free}  "
                        f"历史最低 {_fmt_bytes(int(kv['psram_min']))}  "
                        f"最大块 {_fmt_bytes(int(kv.get('psram_largest', '0')))}"
                    )
                else:
                    self.sys_vars["psram"].set(f"空闲 {free}")
            if "samples" in kv:
                self.sys_vars["samples"].set(kv["samples"])
            for idx, vals in slots.items():
                if self.tree.exists(str(idx)):
                    self.tree.item(str(idx), values=vals)
        except Exception:
            pass

    def _sd_open_selected(self) -> None:
        sel = self.score_tree.selection()
        if not sel:
            messagebox.showinfo("谱子", "先刷新并选中一行")
            return
        path = self.score_tree.item(sel[0], "values")[0]
        self._opening = True
        self._send("score open " + path)
        self.after(20000, lambda: setattr(self, "_opening", False))

    def _hid_note(self, msg: str) -> None:
        self.hid_log.insert("end", msg + "\n")
        try:
            if float(self.hid_log.index("end")) > 80:
                self.hid_log.delete("1.0", "40.0")
        except tk.TclError:
            pass
        self.hid_log.see("end")

    def _on_host_key(self, event: tk.Event) -> str | None:
        w = event.widget
        if w is self.port_box:
            return None
        ch = event.char if event.char and event.char.isprintable() else ""
        self._hid_note(f"host keysym={event.keysym} char={ch!r}")
        if w is self.log or w is self.hid_log:
            return "break"
        return None

    def _tcp_play1(self) -> None:
        def go() -> None:
            try:
                sock = socket.create_connection(("192.168.110.180", 2333), timeout=4)
                sock.sendall(b"play 1\n")
                sock.close()
                self.after(0, lambda: self._log("TCP 已发送 play 1 → 192.168.110.180:2333"))
            except Exception as exc:  # noqa: BLE001
                self.after(0, lambda: messagebox.showerror("TCP", f"先点「开 AP」并让本机连上 S3Keys。\n{exc}"))

        threading.Thread(target=go, daemon=True).start()

    def _apply_scores(self, files: list[tuple[str, str]]) -> None:
        for iid in self.score_tree.get_children():
            self.score_tree.delete(iid)
        for i, (path, sz) in enumerate(files):
            self.score_tree.insert("", "end", iid=str(i), values=(path, sz))
        self.dev_score_info.set(f"列出 {len(files)} 个文件（双击打开，再点播放）")

    def _apply_score_info(self, lines: list[str]) -> None:
        kv: dict[str, str] = {}
        for raw in lines:
            if "=" in raw:
                k, v = raw.split("=", 1)
                kv[k] = v
        self.dev_score_info.set(
            f"已打开 {self._pct(kv.get('title', '-'))}  notes={kv.get('notes', '?')}  "
            f"{self._pct(kv.get('path', ''))}"
        )

    def _handle_line(self, line: str) -> None:
        if "BEGIN_MUSIC" in line:
            self._in_music = True
            self._music_files = []
            return
        if "END_MUSIC" in line:
            self._in_music = False
            self._apply_music(self._music_files)
            return
        if self._in_music:
            raw = line.strip()
            if raw.startswith("file="):
                rest = raw[5:]
                if " bytes=" in rest:
                    path, sz = rest.rsplit(" bytes=", 1)
                    self._music_files.append((self._pct(path), sz))
                else:
                    self._music_files.append((self._pct(rest), "0"))
            return
        if "BEGIN_SCORES" in line:
            self._in_scores = True
            self._list_meta = {}
            return
        if "END_SCORES" in line:
            self._in_scores = False
            self._apply_scores(self._score_files)
            try:
                off = int(self._list_meta.get("offset", "0") or 0)
                shown = int(self._list_meta.get("shown", "0") or 0)
                total = int(self._list_meta.get("total", "0") or 0)
            except ValueError:
                self._listing = False
                return
            nxt = off + shown
            if shown > 0 and nxt < total:
                self.dev_score_info.set(f"已列 {nxt}/{total} …")
                self._send(f"score list {nxt} 80", silent=True)
            else:
                self._listing = False
                if total == 0:
                    self.dev_score_info.set("没有谱子：需要 TF 卡 /sdcard/puzi（拔卡时列表为空是正常的）")
                else:
                    self.dev_score_info.set(
                        f"列出 {len(self._score_files)} / {total or len(self._score_files)} 个文件（双击打开）"
                    )
            return
        if self._in_scores:
            raw = line.strip()
            if raw.startswith("file="):
                rest = raw[5:]
                if " bytes=" in rest:
                    path, sz = rest.rsplit(" bytes=", 1)
                    self._score_files.append((self._pct(path), sz))
            elif "=" in raw:
                for part in raw.split():
                    if "=" not in part:
                        continue
                    k, v = part.split("=", 1)
                    if k in ("offset", "shown", "total") and v.lstrip("-").isdigit():
                        self._list_meta[k] = v
            return
        if "BEGIN_SCORE_INFO" in line:
            self._in_score_info = True
            self._score_info_lines = []
            return
        if "END_SCORE_INFO" in line:
            self._in_score_info = False
            self._opening = False
            self._apply_score_info(self._score_info_lines)
            return
        if self._in_score_info:
            self._score_info_lines.append(line.strip())
            return
        if "BEGIN_STATUS" in line:
            self._in_status = True
            self._status_lines = []
            return
        if "END_STATUS" in line:
            self._in_status = False
            self._apply_status(self._status_lines)
            return
        if self._in_status:
            if len(self._status_lines) < 400:
                self._status_lines.append(line.strip())
            return
        if line.strip().startswith("EVENT "):
            self._hid_note(line.strip())
            return
        if line.strip():
            self._log(line)

    def _pump_rx(self) -> None:
        n = 0
        while n < 64:
            try:
                chunk = self._rx_q.get_nowait()
            except queue.Empty:
                break
            n += 1
            self._rx_buf += chunk.decode("utf-8", errors="replace")
            if len(self._rx_buf) > 262144:
                self._rx_buf = self._rx_buf[-16384:]
            lines = 0
            while "\n" in self._rx_buf and lines < 800:
                line, self._rx_buf = self._rx_buf.split("\n", 1)
                self._handle_line(line.rstrip("\r"))
                lines += 1
        self.after(40, self._pump_rx)

    def _log(self, msg: str) -> None:
        self.log.insert("end", msg + "\n")
        try:
            if float(self.log.index("end")) > 300:
                self.log.delete("1.0", "120.0")
        except tk.TclError:
            pass
        self.log.see("end")

    def _on_close(self) -> None:
        self._score_stop.set()
        self._io_run = False
        self._disconnect()
        self.destroy()


if __name__ == "__main__":
    DebugPad().mainloop()
