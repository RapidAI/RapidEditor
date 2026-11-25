# P3 acceptance: pure keyboard flow — Ctrl+Shift+P palette, type query, Enter.
import ctypes, ctypes.wintypes as wt, subprocess, time, sys, os
from PIL import Image

REPO = r"C:\Users\ma139\WorkBuddy AI\2026-11-15-16-34-53\RapidEditor-repo"
exe = REPO + r"\build-msvc\RapidEditor.exe"
outdir = REPO
target_doc = REPO + r"\build-msvc\acc_cjk.c"

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32

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
    rect = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w, hh = rect.right - rect.left, rect.bottom - rect.top
    hdc = user32.GetWindowDC(hwnd)
    mem = gdi32.CreateCompatibleDC(hdc)
    bmp = gdi32.CreateCompatibleBitmap(hdc, w, hh)
    gdi32.SelectObject(mem, bmp)
    user32.PrintWindow(hwnd, mem, 2)
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
    gdi32.DeleteObject(bmp); gdi32.DeleteDC(mem); user32.ReleaseDC(hwnd, hdc)
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
type_unicode("显示行号")
time.sleep(0.8)
capture(p, hwnd, "acc_pal_query.png")

# 3) Enter runs the top match — the line-number toggle has a visible effect
send_key(0x0D)                  # Enter
time.sleep(0.6)
capture(p, hwnd, "acc_pal_done.png")

p.terminate()
print("done")
