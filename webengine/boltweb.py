#!/usr/bin/env python3
# ===========================================================================
#  BoltOS Web Engine  -  webengine/boltweb.py
#
#  A real Firefox for BoltOS. Pages need the whole web platform -- a
#  JavaScript JIT for megabytes of React, CSS as Firefox does it, video
#  through Media Source Extensions -- and that is a browser engine, not
#  something a 1 GHz board grows. So Firefox runs here, in a Linux VM set up
#  to pass for Firefox on Windows 11 (user.js, fonts.conf), and BoltOS shows
#  what it draws: this program captures Firefox's screen, encodes what
#  changed as H.264 and sends it, and puts the clicks, keys and wheel BoltOS
#  sends back into Firefox as if they were typed here.
#
#  The pieces: an X server with no screen (Xvfb) exactly the size of the
#  BoltOS Browser; Firefox's window filling it; WebDriver BiDi (Firefox's own remote protocol) for addresses, back,
#  forward and what the page is called; ffmpeg (PyAV) to grab the screen
#  and libx264 to encode it; XTest for the input.
#
#  Encoding is shaped for Mini's decoder (mini/h264.c) on a Cortex-A53:
#  baseline profile (CAVLC, no B frames, so every frame shows the moment it
#  is decoded), no deblocking (sharper text, and less work), one reference.
#  Nothing is sent while the screen stands still. Each frame is acknowledged
#  once it is on BoltOS's screen, and no more than WINDOW are ever on their
#  way, so a slow decoder gets fewer, newer frames instead of a queue.
#
#  The protocol, on TCP port 7310. Each message is a type byte, a 32-bit
#  little-endian length, and that many bytes.
#
#   BoltOS -> engine
#    0x01 HELLO     u16 version, u16 width, u16 height, u16 flags, key
#    0x02 NAV       an address (UTF-8)
#    0x03 BACK  0x04 FORWARD  0x05 RELOAD  0x06 STOP
#    0x10 MOUSE     i16 x, i16 y, u8 buttons (1 left, 2 right, 4 middle)
#    0x11 WHEEL     i8 dy (+ is up), i8 dx (+ is right)
#    0x12 KEY       u32 X keysym, u8 modifiers (1 shift, 2 ctrl, 4 alt, 8 super)
#    0x20 ACK       u32 the last frame shown
#    0x21 KEYFRAME  (the decoder lost its place)
#
#   engine -> BoltOS
#    0x81 HELLO     u16 width, u16 height, "Firefox <version>"
#    0x82 CONFIG    an avcC record: the stream's parameter sets
#    0x83 FRAME     u32 number, u8 keyframe, one access unit (4-byte lengths)
#    0x84 STATE     u8 loading, u8 can go back, u8 can go forward, u8 0,
#                   address, NUL, title (UTF-8)
#    0x85 CURSOR    u16 w, u16 h, u16 hot x, u16 hot y, w*h ARGB (LE u32)
#    0x86 ERROR     why (UTF-8); the connection closes after it
#    0x87 AUDIO     what Firefox is playing: 48 kHz 16-bit stereo PCM,
#                   little-endian, 20 ms a message -- only to a BoltOS whose
#                   HELLO had flag 1 (protocol 2: BoltOS TV, for the
#                   streaming services; Mini has no sound out and doesn't ask)
#
#  The sound: Firefox plays into a PulseAudio sink with no hardware behind
#  it, and parec reads that sink's monitor. PCM rather than a codec, because
#  it only ever crosses a home network (1.5 Mbit/s) and the TV then needs
#  no decoder for it.
# ===========================================================================
import asyncio, ctypes, ctypes.util, json, logging, os, re, shutil, signal, struct, subprocess, sys, threading, time
from fractions import Fraction

import av
import numpy as np
import websockets
from Xlib import X, XK, display as xdisplay
from Xlib.ext import xtest

PORT = int(os.environ.get("BOLTWEB_PORT", "7310"))
HERE = os.path.dirname(os.path.abspath(__file__))
STATE_DIR = os.path.expanduser("~/.boltweb")
PROFILE = os.path.join(STATE_DIR, "profile")
DISP = os.environ.get("BOLTWEB_DISPLAY", ":99")
BIDI_PORT = 9222
WINDOW = 2                      # frames on their way, not yet shown
CAPTURE_FPS = 60
CRF = os.environ.get("BOLTWEB_CRF", "20")
PROTO = 2
SINK = "boltweb"
AUDIO_BLOCK = 48000 * 4 // 50          # 20 ms of s16le stereo

C_HELLO, C_NAV, C_BACK, C_FORWARD, C_RELOAD, C_STOP = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06
C_MOUSE, C_WHEEL, C_KEY, C_ACK, C_KEYFRAME = 0x10, 0x11, 0x12, 0x20, 0x21
S_HELLO, S_CONFIG, S_FRAME, S_STATE, S_CURSOR, S_ERROR, S_AUDIO = 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87
HELLO_SOUND = 1

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S", stream=sys.stdout)
log = logging.getLogger("boltweb").info


def engine_key():
    try:
        with open(os.path.join(HERE, "key")) as f:
            return f.read().strip()
    except OSError:
        return ""


def firefox_version():
    try:
        out = subprocess.run(["firefox", "--version"], capture_output=True, text=True, timeout=20).stdout
        m = re.search(r"(\d+)\.(\d+)", out)
        return f"{m.group(1)}.0" if m else "140.0"
    except Exception:
        return "140.0"


# ---- H.264 ------------------------------------------------------------------------------------
def split_nals(b):
    """Annex B -> NAL units."""
    starts = [m.end() for m in re.finditer(b"\x00\x00\x01", b)]
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else len(b)
        nal = b[s:e].rstrip(b"\x00")          # a NAL never ends in a zero byte
        if nal:
            out.append(nal)
    return out


def avcc(sps, pps):
    return (bytes([1, sps[1], sps[2], sps[3], 0xFF, 0xE1]) + struct.pack(">H", len(sps)) + sps +
            bytes([1]) + struct.pack(">H", len(pps)) + pps)


def make_encoder(w, h):
    cc = av.CodecContext.create("libx264", "w")
    cc.width, cc.height = w, h
    cc.pix_fmt = "yuv420p"
    cc.time_base = Fraction(1, 1000)
    cc.options = {
        "preset": "veryfast", "tune": "zerolatency", "profile": "baseline", "forced-idr": "1",
        "x264-params": f"keyint=infinite:scenecut=0:bframes=0:ref=1:no-deblock=1:repeat-headers=1:aud=0:crf={CRF}:threads=2",
    }
    return cc


class Streamer(threading.Thread):
    """Grabs the screen, and encodes and sends it when it changed and the window allows."""

    def __init__(self, w, h, send):
        super().__init__(daemon=True)
        self.w, self.h, self.send = w, h, send
        self.sent = 0                   # the last frame number sent
        self.acked = 0                  # the last one BoltOS showed
        self.want_key = True
        self.halt = False
        self.bytes = 0

    def ack(self, n):
        if n > self.acked:
            self.acked = n

    def run(self):
        try:
            self.loop()
        except Exception as e:
            log(f"capture stopped: {e!r}")

    def loop(self):
        grab = av.open(f"{DISP}.0+0,0", format="x11grab",
                       options={"video_size": f"{self.w}x{self.h}", "framerate": str(CAPTURE_FPS), "draw_mouse": "0"})
        enc = make_encoder(self.w, self.h)
        last = None                     # the picture last sent, as captured
        sps = pps = None
        t0 = time.monotonic()
        stat_t, stat_n = t0, 0
        try:
            for packet in grab.demux(video=0):
                if self.halt:
                    break
                if packet.size == 0:
                    continue
                # bgr0: the fourth byte is padding, and not always the same
                raw = np.frombuffer(packet, dtype=np.uint32) & 0x00FFFFFF
                key = self.want_key
                if not key and (self.sent - self.acked >= WINDOW or (last is not None and np.array_equal(raw, last))):
                    continue
                for frame in packet.decode():
                    yuv = frame.reformat(format="yuv420p")
                    yuv.pts = int((time.monotonic() - t0) * 1000)
                    yuv.time_base = Fraction(1, 1000)
                    # Every grabbed frame comes marked intra, and x264 would make each one
                    # a keyframe: say which is which.
                    try:
                        yuv.pict_type = av.video.frame.PictureType.I if key else av.video.frame.PictureType.NONE
                    except AttributeError:
                        yuv.pict_type = "I" if key else "NONE"
                    for p in enc.encode(yuv):
                        nals = split_nals(bytes(p))
                        au = bytearray()
                        is_key = False
                        for n in nals:
                            t = n[0] & 31
                            if t == 7:
                                sps = n
                            elif t == 8:
                                pps = n
                            elif t in (1, 5):
                                is_key |= t == 5
                                au += struct.pack(">I", len(n)) + n
                        if not au:
                            continue
                        if is_key and sps and pps:
                            self.send(S_CONFIG, avcc(sps, pps))
                        self.sent += 1
                        self.bytes += len(au)
                        self.send(S_FRAME, struct.pack("<IB", self.sent, 1 if is_key else 0) + bytes(au))
                        stat_n += 1
                    if key:
                        self.want_key = False
                    last = raw
                now = time.monotonic()
                if now - stat_t >= 10 and stat_n:
                    log(f"stream: {stat_n / (now - stat_t):.1f} fps, {self.bytes * 8 / (now - stat_t) / 1e6:.2f} Mbit/s")
                    stat_t, stat_n, self.bytes = now, 0, 0
        finally:
            grab.close()


# ---- the sound ---------------------------------------------------------------------------------
def pulse_up():
    """A PulseAudio of our own with a sink that goes nowhere, made Firefox's default. True if it's there."""
    if not shutil.which("pulseaudio") or not shutil.which("pactl"):
        return False
    if subprocess.run(["pactl", "info"], capture_output=True).returncode != 0:
        subprocess.run(["pulseaudio", "--start", "--exit-idle-time=-1", "--disallow-exit"], capture_output=True)
        for _ in range(50):
            if subprocess.run(["pactl", "info"], capture_output=True).returncode == 0:
                break
            time.sleep(0.1)
    sinks = subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout
    if SINK not in sinks:
        subprocess.run(["pactl", "load-module", "module-null-sink", f"sink_name={SINK}", "rate=48000", "channels=2",
                        f"sink_properties=device.description={SINK}"], capture_output=True)
    subprocess.run(["pactl", "set-default-sink", SINK], capture_output=True)
    return SINK in subprocess.run(["pactl", "list", "short", "sinks"], capture_output=True, text=True).stdout


class Sound(threading.Thread):
    """Reads what Firefox plays off the sink's monitor and sends it, 20 ms at a time."""

    def __init__(self, send):
        super().__init__(daemon=True)
        self.send, self.halt, self.proc = send, False, None

    def run(self):
        try:
            self.proc = subprocess.Popen(["parec", f"--device={SINK}.monitor", "--format=s16le", "--rate=48000",
                                          "--channels=2", "--latency-msec=20"], stdout=subprocess.PIPE,
                                         stderr=subprocess.DEVNULL)
            while not self.halt:
                block = self.proc.stdout.read(AUDIO_BLOCK)
                if not block:
                    break
                self.send(S_AUDIO, block)
        except Exception as e:
            log(f"sound stopped: {e!r}")
        finally:
            self.stop()

    def stop(self):
        self.halt = True
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()


# ---- the cursor ----------------------------------------------------------------------------------
class XFixesCursorImage(ctypes.Structure):
    _fields_ = [("x", ctypes.c_short), ("y", ctypes.c_short), ("width", ctypes.c_ushort), ("height", ctypes.c_ushort),
                ("xhot", ctypes.c_ushort), ("yhot", ctypes.c_ushort), ("cursor_serial", ctypes.c_ulong),
                ("pixels", ctypes.POINTER(ctypes.c_ulong)), ("atom", ctypes.c_ulong), ("name", ctypes.c_char_p)]


class Cursor:
    """Firefox's pointer, as a picture: arrow, hand, I-beam, whatever the page asked for."""

    def __init__(self):
        self.ok = False
        try:
            self.x11 = ctypes.CDLL(ctypes.util.find_library("X11"))
            self.xf = ctypes.CDLL(ctypes.util.find_library("Xfixes"))
            self.x11.XOpenDisplay.restype = ctypes.c_void_p
            self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
            self.xf.XFixesGetCursorImage.restype = ctypes.POINTER(XFixesCursorImage)
            self.xf.XFixesGetCursorImage.argtypes = [ctypes.c_void_p]
            self.x11.XFree.argtypes = [ctypes.c_void_p]
            self.dpy = self.x11.XOpenDisplay(DISP.encode())
            self.ok = bool(self.dpy)
        except Exception as e:
            log(f"no cursor shapes: {e!r}")
        self.serial = None

    def poll(self):
        """The cursor's picture if it changed since last time, else None."""
        if not self.ok:
            return None
        p = self.xf.XFixesGetCursorImage(self.dpy)
        if not p:
            return None
        try:
            c = p.contents
            if c.cursor_serial == self.serial:
                return None
            self.serial = c.cursor_serial
            w, h = min(c.width, 64), min(c.height, 64)
            px = bytearray()
            for y in range(h):
                for x in range(w):
                    px += struct.pack("<I", c.pixels[y * c.width + x] & 0xFFFFFFFF)
            return struct.pack("<HHHH", w, h, min(c.xhot, w - 1), min(c.yhot, h - 1)) + bytes(px)
        finally:
            self.x11.XFree(p)


# ---- input ----------------------------------------------------------------------------------------
MODKEYS = ((1, XK.XK_Shift_L), (2, XK.XK_Control_L), (4, XK.XK_Alt_L), (8, XK.XK_Super_L))


class Input:
    def __init__(self):
        self.d = xdisplay.Display(DISP)
        self.buttons = 0
        lo, hi = self.d.display.info.min_keycode, self.d.display.info.max_keycode
        mapping = self.d.get_keyboard_mapping(lo, hi - lo + 1)
        self.spare = [lo + i for i, syms in enumerate(mapping) if not any(syms)][-4:]
        self.remapped = {}

    def mouse(self, x, y, buttons):
        xtest.fake_input(self.d, X.MotionNotify, x=x, y=y)
        for bit, btn in ((1, 1), (2, 3), (4, 2)):
            if (buttons ^ self.buttons) & bit:
                xtest.fake_input(self.d, X.ButtonPress if buttons & bit else X.ButtonRelease, btn)
        self.buttons = buttons
        self.d.sync()

    def wheel(self, dy, dx):
        for n, up, down in ((dy, 4, 5), (dx, 7, 6)):
            for _ in range(min(abs(n), 10)):
                b = up if n > 0 else down
                xtest.fake_input(self.d, X.ButtonPress, b)
                xtest.fake_input(self.d, X.ButtonRelease, b)
        self.d.sync()

    def keycode(self, sym):
        """The key that makes sym, and whether it needs Shift; a spare key is given the
        symbol if no key has it (an accented letter, the euro)."""
        kc = self.d.keysym_to_keycode(sym)
        if kc:
            return kc, self.d.keycode_to_keysym(kc, 0) != sym and self.d.keycode_to_keysym(kc, 1) == sym
        if not self.spare:
            return 0, False
        if sym not in self.remapped:
            kc = self.spare[len(self.remapped) % len(self.spare)]
            for s, k in list(self.remapped.items()):
                if k == kc:
                    del self.remapped[s]
            self.d.change_keyboard_mapping(kc, [(sym, sym)])
            self.d.sync()
            time.sleep(0.02)                  # let Firefox see the new map first
            self.remapped[sym] = kc
        return self.remapped[sym], False

    def key(self, sym, mods):
        kc, shift = self.keycode(sym)
        if not kc:
            return
        if shift:
            mods |= 1
        held = [self.d.keysym_to_keycode(k) for bit, k in MODKEYS if mods & bit]
        for m in held:
            xtest.fake_input(self.d, X.KeyPress, m)
        xtest.fake_input(self.d, X.KeyPress, kc)
        xtest.fake_input(self.d, X.KeyRelease, kc)
        for m in reversed(held):
            xtest.fake_input(self.d, X.KeyRelease, m)
        self.d.sync()


# ---- Firefox -------------------------------------------------------------------------------------
class BidiError(Exception):
    pass


class Bidi:
    """WebDriver BiDi, Firefox's remote protocol: JSON over a WebSocket."""

    def __init__(self, on_event):
        self.on_event = on_event
        self.ws = None
        self.next_id = 1
        self.waiting = {}

    async def connect(self, timeout=40):
        end = time.monotonic() + timeout
        while True:
            try:
                self.ws = await websockets.connect(f"ws://localhost:{BIDI_PORT}/session", max_size=None, ping_interval=None)
                break
            except Exception:
                if time.monotonic() > end:
                    raise
                await asyncio.sleep(0.3)
        self.reader = asyncio.create_task(self.read())
        await self.call("session.new", {"capabilities": {"alwaysMatch": {"unhandledPromptBehavior": {"default": "ignore"}}}})

    async def read(self):
        try:
            async for msg in self.ws:
                m = json.loads(msg)
                if m.get("id") in self.waiting:
                    self.waiting.pop(m["id"]).set_result(m)
                elif m.get("type") == "event":
                    try:
                        self.on_event(m["method"], m.get("params", {}))
                    except Exception as e:
                        log(f"event {m.get('method')}: {e!r}")
        except Exception:
            pass
        for f in self.waiting.values():
            if not f.done():
                f.set_exception(BidiError("closed"))
        self.waiting.clear()

    async def call(self, method, params, timeout=15):
        i = self.next_id
        self.next_id += 1
        fut = asyncio.get_running_loop().create_future()
        self.waiting[i] = fut
        await self.ws.send(json.dumps({"id": i, "method": method, "params": params}))
        m = await asyncio.wait_for(fut, timeout)
        if m.get("type") == "error":
            raise BidiError(f"{m.get('error')}: {m.get('message')}")
        return m.get("result", {})

    async def close(self):
        if self.ws:
            await self.ws.close()


USERCHROME = """
/* BoltOS Web Engine: Firefox's whole window is BoltOS's Browser, so there is
 * no window to minimise or close: no caption buttons. */
.titlebar-buttonbox-container { display: none !important; }
"""

GTK_SETTINGS = """[Settings]
gtk-font-name=Segoe UI 9
gtk-theme-name=Adwaita
gtk-application-prefer-dark-theme=0
gtk-cursor-theme-name=whiteglass
gtk-cursor-theme-size=0
"""

TITLE_JS = ("JSON.stringify({u: location.href, t: document.title,"
            " b: window.navigation ? navigation.canGoBack : history.length > 1,"
            " f: window.navigation ? navigation.canGoForward : false})")


class Browser:
    """Xvfb, and Firefox in it, at the size of a BoltOS page."""

    def __init__(self):
        self.w = self.h = 0
        self.xvfb = self.ff = None
        self.bidi = None
        self.ctx = None
        self.version = firefox_version()
        self.url, self.title, self.loading, self.back, self.fwd = "", "", False, False, False
        self.changed = asyncio.Event()
        self.input = None
        self.cursor = None
        self.sound = pulse_up()
        log("sound: " + (f"PulseAudio sink '{SINK}'" if self.sound else "none (no PulseAudio): pages play silently"))
        try:
            with open(os.path.join(STATE_DIR, "last-url")) as f:
                self.url = f.read().strip()
        except OSError:
            pass

    def alive(self):
        return self.xvfb and self.xvfb.poll() is None and self.ff and self.ff.poll() is None

    async def stop(self):
        if self.bidi:
            try:
                await self.bidi.close()
            except Exception:
                pass
        for p in (self.ff, self.xvfb):
            if p and p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
        self.ff = self.xvfb = self.bidi = self.ctx = None

    def write_profile(self):
        os.makedirs(os.path.join(PROFILE, "chrome"), exist_ok=True)
        with open(os.path.join(HERE, "user.js")) as f:
            prefs = f.read().replace("@VERSION@", self.version)
        with open(os.path.join(PROFILE, "user.js"), "w") as f:
            f.write(prefs + '\nuser_pref("toolkit.legacyUserProfileCustomizations.stylesheets", true);\n')
        with open(os.path.join(PROFILE, "chrome", "userChrome.css"), "w") as f:
            f.write(USERCHROME)
        # Firefox remembers its window's size; this one is always the screen's.
        for name in ("xulstore.json", "sessionstore.jsonlz4"):
            try:
                os.remove(os.path.join(PROFILE, name))
            except OSError:
                pass
        gtk = os.path.expanduser("~/.config/gtk-3.0")
        os.makedirs(gtk, exist_ok=True)
        with open(os.path.join(gtk, "settings.ini"), "w") as f:
            f.write(GTK_SETTINGS)

    async def ensure(self, w, h):
        if self.alive() and (w, h) == (self.w, self.h) and self.bidi:
            return
        await self.stop()
        self.w, self.h = w, h
        log(f"starting Firefox {self.version} at {w}x{h}")
        sock = f"/tmp/.X11-unix/X{DISP[1:]}"
        if os.path.exists(sock):
            subprocess.run(["pkill", "-f", f"Xvfb {DISP}"], check=False)
            time.sleep(0.3)
        self.xvfb = subprocess.Popen(["Xvfb", DISP, "-screen", "0", f"{w}x{h}x24", "-dpi", "96", "-nolisten", "tcp",
                                      "-noreset", "+extension", "XFIXES"],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(sock):
                break
            await asyncio.sleep(0.05)
        self.write_profile()
        env = dict(os.environ, DISPLAY=DISP, GTK_THEME="Adwaita:light", MOZ_ENABLE_WAYLAND="0",
                   XCURSOR_THEME="whiteglass", MOZ_CRASHREPORTER_DISABLE="1", TZ=os.environ.get("TZ", "UTC"))
        env.pop("WAYLAND_DISPLAY", None)
        self.ff = subprocess.Popen(["firefox", "--new-instance", "--profile", PROFILE, "--width", str(w), "--height", str(h),
                                    "--remote-debugging-port", str(BIDI_PORT), "--remote-allow-hosts", "localhost",
                                    "about:blank"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.bidi = Bidi(self.event)
        await self.bidi.connect()
        await self.bidi.call("session.subscribe", {"events": [
            "browsingContext.navigationStarted", "browsingContext.load", "browsingContext.domContentLoaded",
            "browsingContext.fragmentNavigated", "browsingContext.historyUpdated", "browsingContext.contextCreated",
            "browsingContext.contextDestroyed", "browsingContext.navigationFailed"]})
        await self.pick_context()
        self.input = Input()
        self.cursor = Cursor()
        await asyncio.sleep(0.5)
        self.place_window()
        if self.url:
            await self.navigate(self.url)

    def place_window(self):
        """No window manager: put Firefox's window over the whole screen, and give it the keys."""
        d = self.input.d
        root = d.screen().root
        for win in root.query_tree().children:
            try:
                cls = win.get_wm_class()
                attrs = win.get_attributes()
            except Exception:
                continue
            if cls and "firefox" in " ".join(cls).lower() and attrs.map_state == X.IsViewable:
                win.configure(x=0, y=0, width=self.w, height=self.h)
                win.set_input_focus(X.RevertToPointerRoot, X.CurrentTime)
        d.sync()

    async def pick_context(self):
        tree = await self.bidi.call("browsingContext.getTree", {"maxDepth": 0})
        ctxs = tree.get("contexts", [])
        if ctxs:
            self.ctx = ctxs[-1]["context"]

    def event(self, method, p):
        if method == "browsingContext.contextCreated" and not p.get("parent"):
            self.ctx = p["context"]                  # a page opened another window: follow it
            asyncio.ensure_future(self.bidi.call("browsingContext.activate", {"context": self.ctx}))
            return
        if method == "browsingContext.contextDestroyed" and p.get("context") == self.ctx:
            asyncio.ensure_future(self.pick_context())
            return
        if p.get("context") != self.ctx:
            return
        url = p.get("url") or self.url
        if method == "browsingContext.navigationStarted":
            self.set_state(url=url, loading=True)
        elif method in ("browsingContext.load", "browsingContext.navigationFailed"):
            self.set_state(url=url, loading=False)
            asyncio.ensure_future(self.refresh())
        else:
            self.set_state(url=url)
            asyncio.ensure_future(self.refresh())

    def set_state(self, **kw):
        changed = False
        for k, v in kw.items():
            if getattr(self, k) != v:
                setattr(self, k, v)
                changed = True
        if changed:
            self.changed.set()
            if "url" in kw and self.url.startswith("http"):
                try:
                    with open(os.path.join(STATE_DIR, "last-url"), "w") as f:
                        f.write(self.url)
                except OSError:
                    pass

    async def refresh(self):
        """The page's address, title and history, asked of the page itself."""
        if not self.bidi or not self.ctx:
            return
        try:
            r = await self.bidi.call("script.evaluate", {"expression": TITLE_JS, "awaitPromise": False,
                                                         "target": {"context": self.ctx}}, timeout=5)
            v = json.loads(r.get("result", {}).get("value", "{}"))
            self.set_state(url=v.get("u", self.url), title=v.get("t", ""), back=bool(v.get("b")), fwd=bool(v.get("f")))
        except Exception:
            pass

    async def navigate(self, url):
        self.set_state(url=url, loading=True)
        try:
            await self.bidi.call("browsingContext.navigate", {"context": self.ctx, "url": url, "wait": "none"})
        except BidiError as e:
            log(f"navigate {url}: {e}")
            self.set_state(loading=False)

    async def history(self, delta):
        try:
            await self.bidi.call("browsingContext.traverseHistory", {"context": self.ctx, "delta": delta})
        except BidiError:
            pass

    async def reload(self):
        try:
            await self.bidi.call("browsingContext.reload", {"context": self.ctx, "wait": "none"})
        except BidiError:
            pass

    async def stop_loading(self):
        try:
            await self.bidi.call("script.evaluate", {"expression": "window.stop()", "awaitPromise": False,
                                                     "target": {"context": self.ctx}})
        except BidiError:
            pass


# ---- BoltOS, on the other end -------------------------------------------------------------------
class Session:
    current = None

    def __init__(self, browser, reader, writer):
        self.b, self.r, self.w = browser, reader, writer
        self.streamer = None
        self.sound = None
        self.loop = asyncio.get_running_loop()
        self.closed = False
        self.peer = writer.get_extra_info("peername")

    def send(self, kind, payload=b""):
        if self.closed:
            return
        self.w.write(struct.pack("<BI", kind, len(payload)) + payload)

    def send_threadsafe(self, kind, payload):
        self.loop.call_soon_threadsafe(self.send, kind, payload)

    def send_state(self):
        b = self.b
        self.send(S_STATE, bytes([1 if b.loading else 0, 1 if b.back else 0, 1 if b.fwd else 0, 0]) +
                  b.url.encode()[:2000] + b"\0" + b.title.encode()[:400])

    async def read_msg(self):
        hdr = await self.r.readexactly(5)
        kind, n = struct.unpack("<BI", hdr)
        if n > 1 << 20:
            raise ValueError("message too long")
        return kind, await self.r.readexactly(n) if n else b""

    async def run(self):
        kind, p = await asyncio.wait_for(self.read_msg(), 10)
        if kind != C_HELLO or len(p) < 8:
            return
        ver, w, h, flags = struct.unpack("<HHHH", p[:8])
        key = engine_key()
        if key and p[8:].decode(errors="replace") != key:
            log(f"{self.peer}: wrong key")
            self.send(S_ERROR, b"The engine's key doesn't match")
            await self.w.drain()
            return
        w, h = max(320, min(w & ~1, 3840)), max(240, min(h & ~1, 2160))
        if Session.current:
            Session.current.send(S_ERROR, b"Another BoltOS is using the engine now")
            Session.current.close()
        Session.current = self
        log(f"{self.peer}: BoltOS at {w}x{h} (protocol {ver})")
        await self.b.ensure(w, h)
        self.send(S_HELLO, struct.pack("<HH", w, h) + f"Firefox {self.b.version}".encode())
        self.send_state()
        await self.b.refresh()
        self.streamer = Streamer(w, h, self.send_threadsafe)
        self.streamer.start()
        if ver >= 2 and flags & HELLO_SOUND and self.b.sound:
            self.sound = Sound(self.send_threadsafe)
            self.sound.start()
        tasks = [asyncio.create_task(self.watch_state()), asyncio.create_task(self.watch_cursor())]
        try:
            while not self.closed:
                kind, p = await self.read_msg()
                await self.handle(kind, p)
        finally:
            for t in tasks:
                t.cancel()

    async def handle(self, kind, p):
        b, inp = self.b, self.b.input
        if kind == C_MOUSE and len(p) >= 5:
            x, y, btn = struct.unpack("<hhB", p[:5])
            inp.mouse(max(0, min(x, b.w - 1)), max(0, min(y, b.h - 1)), btn)
        elif kind == C_WHEEL and len(p) >= 2:
            dy, dx = struct.unpack("<bb", p[:2])
            inp.wheel(dy, dx)
        elif kind == C_KEY and len(p) >= 5:
            sym, mods = struct.unpack("<IB", p[:5])
            inp.key(sym, mods)
        elif kind == C_ACK and len(p) >= 4:
            self.streamer.ack(struct.unpack("<I", p[:4])[0])
        elif kind == C_KEYFRAME:
            self.streamer.want_key = True
        elif kind == C_NAV:
            await b.navigate(p.decode(errors="replace"))
        elif kind == C_BACK:
            await b.history(-1)
        elif kind == C_FORWARD:
            await b.history(1)
        elif kind == C_RELOAD:
            await b.reload()
        elif kind == C_STOP:
            await b.stop_loading()

    async def watch_state(self):
        n = 0
        while not self.closed:
            try:
                await asyncio.wait_for(self.b.changed.wait(), 1.0)
            except asyncio.TimeoutError:
                n += 1
                if n % 2 == 0:
                    await self.b.refresh()         # titles change without an event
                continue
            self.b.changed.clear()
            self.send_state()
            await self.w.drain()

    async def watch_cursor(self):
        while not self.closed:
            await asyncio.sleep(0.04)
            c = self.b.cursor.poll() if self.b.cursor else None
            if c:
                self.send(S_CURSOR, c)

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.streamer:
            self.streamer.halt = True
        if self.sound:
            self.sound.stop()
        try:
            self.w.close()
        except Exception:
            pass
        if Session.current is self:
            Session.current = None


async def main():
    os.makedirs(STATE_DIR, exist_ok=True)
    browser = Browser()
    log(f"BoltOS Web Engine on port {PORT}, Firefox {browser.version}{'' if engine_key() else ', no key (anyone may connect)'}")

    async def client(reader, writer):
        s = Session(browser, reader, writer)
        try:
            await s.run()
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError):
            pass
        except Exception as e:
            log(f"{s.peer}: {e!r}")
            if not browser.alive():
                await browser.stop()
        finally:
            log(f"{s.peer}: gone")
            s.close()

    server = await asyncio.start_server(client, "0.0.0.0", PORT)
    stop = asyncio.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        asyncio.get_running_loop().add_signal_handler(sig, stop.set)
    async with server:
        await stop.wait()
    await browser.stop()


if __name__ == "__main__":
    asyncio.run(main())
