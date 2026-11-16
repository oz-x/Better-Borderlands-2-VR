# Foreground helpers that do not synthesise ALT (which can pop the Start menu).
import ctypes
user32, kernel32 = ctypes.windll.user32, ctypes.windll.kernel32
user32.SetProcessDPIAware()

def game_window():
    return user32.FindWindowW(None, "Borderlands 2 (32-bit, DX9)")

def focus(hwnd):
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return fg
    me = kernel32.GetCurrentThreadId()
    other = user32.GetWindowThreadProcessId(fg, None)
    user32.AttachThreadInput(me, other, True)
    user32.BringWindowToTop(hwnd)
    user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(me, other, False)
    return fg
