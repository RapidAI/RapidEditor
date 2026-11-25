# P3 acceptance: pure keyboard flow — Ctrl+Shift+P palette, type query, Enter.
import ctypes, ctypes.wintypes as wt, subprocess, time, sys, os
from PIL import Image

REPO = r"C:\Users\ma139\WorkBuddy AI\2026-11-15-16-34-53\RapidEditor-repo"
exe = REPO + r"\build-msvc\RapidEditor.exe"
outdir = REPO
target_doc = REPO + r"\build-msvc\acc_cjk.c"

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

# DPI-aware: window rects and desktop grabs both work in physical pixels
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # PMv2
except Exception:
    try: user32.SetProcessDPIAware()
    except Exception: pass

def send_key(vk, mods=()):           # press mods, tap vk, release
    for m in mods: user32.keybd_event(m, 0, 0, 0)
    user32.keybd_event(vk, 0, 0, 0)
    user32.keybd_event(vk, 0, 2, 0)
    for m in reversed(mods): user32.keybd_event(m, 0, 2, 0)

def type_unicode(s):
    class KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", wt.WORD), ("wScan", wt.WORD), ("dwFlags", wt.DWORD),
                    ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_ulonglong)]
    class MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", wt.LONG), ("dy", wt.LONG), ("mouseData", wt.DWORD),
                    ("dwFlags", wt.DWORD), ("time", wt.DWORD), ("dwExtraInfo", ctypes.c_ulonglong)]
    class _U(ctypes.Union):
        _fields_ = [("ki", KEYBDINPUT), ("mi", MOUSEINPUT)]
    class INPUT(ctypes.Structure):
        _anonymous_ = ("u",)
        _fields_ = [("type", wt.DWORD), ("u", _U)]
    inputs = (INPUT * (len(s) * 2))()
    for i, ch in enumerate(s):
        for j, up in ((2 * i, False), (2 * i + 1, True)):
            inputs[j].type = 1
            inputs[j].ki = KEYBDINPUT(0, ord(ch), 4 | (2 if up else 0), 0, 0)
    n = user32.SendInput(len(inputs), inputs, ctypes.sizeof(INPUT))
    print("SendInput ->", n, "of", len(inputs), "size", ctypes.sizeof(INPUT))

def capture(p, hwnd, name):
    # Desktop BitBlt cropped to the window rect. PrintWindow (even with
    # PW_RENDERFULLCONTENT) returns stale content for this GL window — it can
    # miss toasts and even the status bar — so grab the desktop instead.
    # Requires a DPI-aware process so rect and pixels are both physical.
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, hh = rect.right - rect.left, rect.bottom - rect.top
    sdc = user32.GetDC(0)
    mem = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, hh)
    gdi32.SelectObject(mem, bmp)
    gdi32.BitBlt(mem, 0, 0, w, hh, sdc, rect.left, rect.top, 0x00CC0020)  # SRCCOPY
    class BIH(ctypes.Structure):
        _pack_ = 1
        _fields_ = [("biSize", ctypes.c_uint32), ("biWidth", ctypes.c_int32),
            ("biHeight", ctypes.c_int32), ("biPlanes", ctypes.c_uint16),
            ("biBitCount", ctypes.c_uint16), ("biCompression", ctypes.c_uint32),
            ("biSizeImage", ctypes.c_uint32), ("biXPelsPerMeter", ctypes.c_int32),
            ("biYPelsPerMeter", ctypes.c_int32), ("biClrUsed", ctypes.c_uint32),
            ("biClrImportant", ctypes.c_uint32)]
    bmi = BIH(ctypes.sizeof(BIH), w, -hh, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * hh * 4)
    gdi32.GetDIBits(mem, bmp, 0, hh, buf, ctypes.byref(bmi), 0)
    img = Image.frombuffer("RGBA", (w, hh), buf, "raw", "BGRA", 0, 1).convert("RGB")
    out = os.path.join(outdir, name)
    img.save(out)
    gdi32.DeleteObject(bmp); gdi32.DeleteDC(mem); user32.ReleaseDC(0, sdc)
    return out

p = subprocess.Popen([exe, target_doc])
time.sleep(4)
hwnd = None
@ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
def cb(h, l):
    global hwnd
    wpid = wt.DWORD()
    user32.GetWindowThreadProcessId(h, ctypes.byref(wpid))
    if wpid.value == p.pid and user32.IsWindowVisible(h):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, buf, 256)
        if buf.value: hwnd = h; return False
    return True
user32.EnumWindows(cb, 0)
if not hwnd: print("FAIL: no window"); p.terminate(); sys.exit(1)
user32.ShowWindow(hwnd, 3)      # maximize
user32.SetForegroundWindow(hwnd)
time.sleep(1)

# 1) Ctrl+Shift+P opens the command palette
send_key(0x50, (0x11, 0x10))    # P with Ctrl+Shift
time.sleep(0.8)
capture(p, hwnd, "acc_pal_open.png")

# 2) type the query (Chinese, via KEYEVENTF_UNICODE)
type_unicode("排序")
time.sleep(0.8)
capture(p, hwnd, "acc_pal_query.png")

# 3) Enter runs the top match — toast should pop at the bottom-right corner
send_key(0x0D)                  # Enter
capture(p, hwnd, "acc_pre_enter.png")   # baseline right after Enter dispatch
marks = (0.2, 0.5, 1.0, 2.0, 3.5, 4.6)
prev = 0.0
for i, t in enumerate(marks):
    time.sleep(t - prev)
    prev = t
    capture(p, hwnd, f"acc_t{i}.png")

from PIL import ImageChops
base = Image.open(outdir + r"\acc_pre_enter.png").convert("RGB")
for i in range(6):
    im = Image.open(outdir + rf"\acc_t{i}.png").convert("RGB")
    print(f"t{i} ({marks[i]}s) diff bbox:", ImageChops.difference(base, im).getbbox())

p.terminate()
print("done")
