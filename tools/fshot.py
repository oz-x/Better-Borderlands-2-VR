# Focuses the game window briefly, captures it from the screen, restores focus.
import ctypes, time, sys
from PIL import ImageGrab
from ctypes import wintypes
user32 = ctypes.windll.user32; user32.SetProcessDPIAware()
hwnd = user32.FindWindowW(None, "Borderlands 2 (32-bit, DX9)")
prev = user32.GetForegroundWindow()
from fg import focus; focus(hwnd); time.sleep(0.8)
r = wintypes.RECT(); user32.GetWindowRect(hwnd, ctypes.byref(r))
img = ImageGrab.grab(bbox=(r.left, r.top, r.right, r.bottom), all_screens=True)
focus(prev)
img.thumbnail((int(sys.argv[2]) if len(sys.argv) > 2 else 700,)*2); img.save(sys.argv[1] if len(sys.argv) > 1 else 'build/win.png')
