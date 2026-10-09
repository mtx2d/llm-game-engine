"""Drive a relocated shipping game through real Win32 input and client pixels.

Requires an interactive Windows desktop. Input is refused unless the launched
process owns both the foreground window and keyboard focus; no keyboard messages
are posted as a substitute for SendInput.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from BlockStackTests import state
from ExportTests import export, package_executable


# Windows uses LLP64. Explicit widths keep these layouts correct on Win64 and
# allow their ABI assertions to run on a Linux host without loading Win32 DLLs.
# https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-input
# https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-keybdinput
DWORD = ctypes.c_uint32
WORD = ctypes.c_uint16
LONG = ctypes.c_int32
BOOL = ctypes.c_int32
HANDLE = ctypes.c_void_p
ULONG_PTR = ctypes.c_size_t
LONG_PTR = ctypes.c_ssize_t


class Rectangle(ctypes.Structure):
    _fields_ = [("left", LONG), ("top", LONG), ("right", LONG), ("bottom", LONG)]


class KeyboardInput(ctypes.Structure):
    _fields_ = [("virtual_key", WORD), ("scan_code", WORD), ("flags", DWORD),
                ("time", DWORD), ("extra_info", ULONG_PTR)]


class MouseInput(ctypes.Structure):
    _fields_ = [("x", LONG), ("y", LONG), ("data", DWORD), ("flags", DWORD),
                ("time", DWORD), ("extra_info", ULONG_PTR)]


class HardwareInput(ctypes.Structure):
    _fields_ = [("message", DWORD), ("low", WORD), ("high", WORD)]


class InputValue(ctypes.Union):
    # MOUSEINPUT determines INPUT's union size even when sending only keys.
    _fields_ = [("mouse", MouseInput), ("keyboard", KeyboardInput), ("hardware", HardwareInput)]


class Input(ctypes.Structure):
    _fields_ = [("type", DWORD), ("value", InputValue)]


class GuiThreadInfo(ctypes.Structure):
    _fields_ = [("size", DWORD), ("flags", DWORD), ("active", HANDLE), ("focus", HANDLE),
                ("capture", HANDLE), ("menu_owner", HANDLE), ("move_size", HANDLE),
                ("caret", HANDLE), ("caret_rectangle", Rectangle)]


class BitmapInfoHeader(ctypes.Structure):
    _fields_ = [("size", DWORD), ("width", LONG), ("height", LONG), ("planes", WORD),
                ("bit_count", WORD), ("compression", DWORD), ("image_size", DWORD),
                ("x_pixels_per_meter", LONG), ("y_pixels_per_meter", LONG),
                ("colors_used", DWORD), ("colors_important", DWORD)]


class BitmapInfo(ctypes.Structure):
    _fields_ = [("header", BitmapInfoHeader), ("colors", DWORD * 1)]


def validate_layouts():
    wide = ctypes.sizeof(HANDLE) == 8
    assert ctypes.sizeof(KeyboardInput) == (24 if wide else 16)
    assert ctypes.sizeof(MouseInput) == (32 if wide else 24)
    assert ctypes.sizeof(Input) == (40 if wide else 28)
    assert Input.value.offset == (8 if wide else 4)
    assert KeyboardInput.extra_info.offset == (16 if wide else 12)
    assert ctypes.sizeof(GuiThreadInfo) == (72 if wide else 48)
    assert ctypes.sizeof(Rectangle) == 16 and ctypes.sizeof(BitmapInfoHeader) == 40


class NativeWindow:
    def __init__(self, process):
        if os.name != "nt":
            raise RuntimeError("Win32 native input requires an interactive Windows desktop")
        validate_layouts()
        self.process = process
        self.window = None
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.gdi = ctypes.WinDLL("gdi32", use_last_error=True)
        self.enumerator = ctypes.WINFUNCTYPE(BOOL, HANDLE, LONG_PTR)

        def bind(library, name, result, *arguments):
            function = getattr(library, name)
            function.restype = result
            function.argtypes = list(arguments)

        bind(self.user, "EnumWindows", BOOL, self.enumerator, LONG_PTR)
        bind(self.user, "GetWindowThreadProcessId", DWORD, HANDLE, ctypes.POINTER(DWORD))
        bind(self.user, "IsWindowVisible", BOOL, HANDLE)
        bind(self.user, "IsIconic", BOOL, HANDLE)
        bind(self.user, "GetClientRect", BOOL, HANDLE, ctypes.POINTER(Rectangle))
        bind(self.user, "ShowWindow", BOOL, HANDLE, ctypes.c_int)
        bind(self.user, "SetForegroundWindow", BOOL, HANDLE)
        bind(self.user, "GetForegroundWindow", HANDLE)
        bind(self.user, "GetGUIThreadInfo", BOOL, DWORD, ctypes.POINTER(GuiThreadInfo))
        bind(self.user, "GetAsyncKeyState", ctypes.c_int16, ctypes.c_int)
        bind(self.user, "SendInput", DWORD, DWORD, ctypes.POINTER(Input), ctypes.c_int)
        bind(self.user, "PostMessageW", BOOL, HANDLE, DWORD, ULONG_PTR, LONG_PTR)
        bind(self.user, "GetDC", HANDLE, HANDLE)
        bind(self.user, "ReleaseDC", ctypes.c_int, HANDLE, HANDLE)
        bind(self.user, "SetThreadDpiAwarenessContext", HANDLE, HANDLE)
        bind(self.gdi, "CreateCompatibleDC", HANDLE, HANDLE)
        bind(self.gdi, "CreateCompatibleBitmap", HANDLE, HANDLE, ctypes.c_int, ctypes.c_int)
        bind(self.gdi, "SelectObject", HANDLE, HANDLE, HANDLE)
        bind(self.gdi, "BitBlt", BOOL, HANDLE, ctypes.c_int, ctypes.c_int, ctypes.c_int,
             ctypes.c_int, HANDLE, ctypes.c_int, ctypes.c_int, DWORD)
        bind(self.gdi, "GetDIBits", ctypes.c_int, HANDLE, HANDLE, DWORD, DWORD,
             ctypes.c_void_p, ctypes.POINTER(BitmapInfo), DWORD)
        bind(self.gdi, "DeleteObject", BOOL, HANDLE)
        bind(self.gdi, "DeleteDC", BOOL, HANDLE)
        # Per-monitor v2 prevents DPI virtualization of the captured client size.
        self.previous_dpi = self.user.SetThreadDpiAwarenessContext(HANDLE(-4))
        self.check(self.previous_dpi, "SetThreadDpiAwarenessContext")

    @staticmethod
    def check(result, operation):
        if not result:
            raise OSError(ctypes.get_last_error(), f"Win32 {operation} failed")
        return result

    def close(self):
        if self.previous_dpi:
            self.check(self.user.SetThreadDpiAwarenessContext(self.previous_dpi),
                       "restore thread DPI context")
            self.previous_dpi = None

    def owned_thread(self, window):
        process_id = DWORD()
        thread = self.user.GetWindowThreadProcessId(window, ctypes.byref(process_id))
        return thread if process_id.value == self.process.pid else 0

    def require_alive(self):
        assert self.process.poll() is None, "Relocated runtime exited during native interaction"

    def find(self, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.require_alive()
            candidates = []

            @self.enumerator
            def visit(window, parameter):
                rectangle = Rectangle()
                if self.owned_thread(window) and self.user.IsWindowVisible(window) and \
                        self.user.GetClientRect(window, ctypes.byref(rectangle)):
                    area = (rectangle.right - rectangle.left) * (rectangle.bottom - rectangle.top)
                    if area > 0:
                        candidates.append((area, window))
                return 1

            self.check(self.user.EnumWindows(visit, 0), "EnumWindows")
            if candidates:
                self.window = max(candidates)[1]
                return
            time.sleep(0.02)
        raise AssertionError("Relocated runtime did not create a visible window owned by its process")

    def focused(self):
        if not self.window or self.user.GetForegroundWindow() != self.window:
            return False
        thread = self.owned_thread(self.window)
        if not thread or not self.user.IsWindowVisible(self.window) or self.user.IsIconic(self.window):
            return False
        info = GuiThreadInfo()
        info.size = ctypes.sizeof(info)
        self.check(self.user.GetGUIThreadInfo(thread, ctypes.byref(info)), "GetGUIThreadInfo")
        return info.active == self.window and info.focus == self.window and not (info.flags & 0x1E)

    def focus(self):
        assert self.owned_thread(self.window), "Refusing to focus a window from another process"
        self.user.ShowWindow(self.window, 9)  # SW_RESTORE; return value is previous visibility.
        self.user.SetForegroundWindow(self.window)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            self.require_alive()
            if self.focused():
                return
            time.sleep(0.02)
        raise AssertionError("Cannot focus the launched game; refusing to send input to another window")

    def send_space(self):
        self.require_alive()
        assert self.focused(), "Game lost foreground keyboard focus; no input was sent"
        for key in (0x20, 0x10, 0x11, 0x12, 0x5B, 0x5C):
            assert not self.user.GetAsyncKeyState(key) & 0x8000, \
                f"Key 0x{key:02X} is already held; refusing to change existing keyboard state"
        inputs = (Input * 2)()
        for index, flags in enumerate((0x0008, 0x0008 | 0x0002)):
            inputs[index].type = 1  # INPUT_KEYBOARD
            inputs[index].value.keyboard = KeyboardInput(0, 0x39, flags, 0, 0)
        # SendInput serializes this physical Space down/up pair without another
        # injected input between them. InputState preserves both edges in one poll.
        sent = self.user.SendInput(2, inputs, ctypes.sizeof(Input))
        if sent != 2:
            error = ctypes.get_last_error()
            if sent == 1 and self.focused():
                # Only repair our partially inserted key while our window owns focus.
                self.check(self.user.SendInput(1, ctypes.byref(inputs[1]), ctypes.sizeof(Input)),
                           "release partially inserted Space")
            raise OSError(error, f"SendInput inserted {sent}/2 events (desktop/UIPI restrictions may apply)")
        assert self.focused(), "Game lost foreground focus during SendInput"

    def capture(self):
        self.require_alive()
        assert self.focused(), "Game lost foreground focus while reading its client pixels"
        rectangle = Rectangle()
        self.check(self.user.GetClientRect(self.window, ctypes.byref(rectangle)), "GetClientRect")
        width, height = rectangle.right - rectangle.left, rectangle.bottom - rectangle.top
        assert width > 0 and height > 0 and width * height <= 16 * 1024 * 1024, \
            "Invalid native window image dimensions"
        source = self.check(self.user.GetDC(self.window), "GetDC")
        memory = bitmap = previous = None
        try:
            memory = self.check(self.gdi.CreateCompatibleDC(source), "CreateCompatibleDC")
            bitmap = self.check(self.gdi.CreateCompatibleBitmap(source, width, height), "CreateCompatibleBitmap")
            previous = self.check(self.gdi.SelectObject(memory, bitmap), "SelectObject")
            self.check(self.gdi.BitBlt(memory, 0, 0, width, height, source, 0, 0, 0x00CC0020), "BitBlt")
            # GetDIBits requires its bitmap to be deselected from every DC.
            self.check(self.gdi.SelectObject(memory, previous), "deselect capture bitmap")
            previous = None
            info = BitmapInfo()
            info.header = BitmapInfoHeader(ctypes.sizeof(BitmapInfoHeader), width, -height,
                                           1, 32, 0, width * height * 4, 0, 0, 0, 0)
            buffer = ctypes.create_string_buffer(width * height * 4)
            lines = self.gdi.GetDIBits(source, bitmap, 0, height, buffer, ctypes.byref(info), 0)
            assert lines == height, f"GetDIBits returned {lines}/{height} scan lines"
            bgra = buffer.raw
            # The unused GDI alpha byte is unspecified; it cannot establish a
            # frame change. Read only client RGB, excluding title bars and cursor.
            rgb = bytearray(width * height * 3)
            rgb[0::3], rgb[1::3], rgb[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
            return width, height, bytes(rgb)
        finally:
            if previous:
                self.gdi.SelectObject(memory, previous)
            if bitmap:
                self.gdi.DeleteObject(bitmap)
            if memory:
                self.gdi.DeleteDC(memory)
            self.user.ReleaseDC(self.window, source)

    def request_close(self):
        self.require_alive()
        assert self.owned_thread(self.window), "Refusing to close another process's window"
        self.check(self.user.PostMessageW(self.window, 0x0010, 0, 0), "WM_CLOSE")


def wait_for_image(window, previous=None, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        width, height, pixels = window.capture()
        digest = hashlib.sha256(pixels).digest()
        if len(set(pixels)) > 32 and digest != previous:
            return digest, (width, height, pixels)
        time.sleep(0.02)
    raise AssertionError("Game did not present a populated changed client image")


def save_image(path, image):
    width, height, pixels = image
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "runtime", "assets", "notices", "artifacts"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    if os.name != "nt":
        raise RuntimeError("WindowsRuntimeInput requires an interactive Windows desktop")
    editor, runtime, assets, notices, artifacts = (getattr(args, name).resolve()
        for name in ("editor", "runtime", "assets", "notices", "artifacts"))
    artifacts.mkdir(parents=True, exist_ok=True)
    output = artifacts / "WindowsRuntimeInput.aster"
    stdout_path = artifacts / "WindowsRuntimeInput.json"
    stderr_path = artifacts / "WindowsRuntimeInput.stderr"
    before_path, after_path = (artifacts / f"WindowsRuntimeInput{phase}.ppm" for phase in ("Before", "After"))
    for path in (output, before_path, after_path):
        path.unlink(missing_ok=True)

    with tempfile.TemporaryDirectory(prefix="AsterWindowsInput-") as temporary:
        root = Path(temporary)
        source, package = root / "Source Assets", root / "Exported Game"
        shutil.copytree(assets, source)
        response = export(editor, runtime, notices, source, package, scene="Games/BlockStack/LineClear.aster")
        assert response.get("ok"), f"Win32 input fixture export failed: {response}"
        moved = root / "Relocated Game With Spaces"
        package.rename(moved)
        shutil.rmtree(source)
        elsewhere = root / "Unrelated Working Directory"
        elsewhere.mkdir()
        assert not source.exists() and not package.exists(), "Source paths survived game relocation"
        with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
            process = subprocess.Popen([str(package_executable(moved)), "--window", "--steps", "10000",
                                        "--validation", "--output", str(output)],
                                       cwd=elsewhere, stdout=stdout, stderr=stderr)
            window = None
            try:
                window = NativeWindow(process)
                window.find()
                window.focus()
                first, image = wait_for_image(window)
                # Send immediately after observing the initial game so gravity
                # cannot change the authored 18-cell hard-drop distance.
                window.send_space()
                changed, after = wait_for_image(window, first)
                assert changed != first
                window.request_close()
                assert process.wait(timeout=30) == 0, "Runtime reported gameplay or Vulkan validation errors"
                save_image(before_path, image)
                save_image(after_path, after)
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                if window:
                    window.close()
                stdout.flush()
                stderr.flush()
                diagnostics = stderr_path.read_text()
                if diagnostics:
                    print(diagnostics)

        result = json.loads(stdout_path.read_text())
        assert result["errors"] == [], f"Exported native gameplay script errors: {result}"
        assert result["frames"] == result["steps"] and 2 <= result["frames"] < 600, \
            "Runtime must present frames and close before gravity alone can win"
        assert "BlockStack: ready" in result["log"] and "BlockStack: victory" in result["log"], \
            "Native exported game did not initialize and reach victory"
        final_scene = json.loads(output.read_text())
        victory = state(final_scene)
        assert victory["status"] == "won" and victory["lines"] == 2 and victory["score"] == 336, \
            f"Native Space did not produce the authored two-row, 336-point victory: {victory}"
        assert "BlockStack: cleared 2 rows; score=336" in result["log"], "Victory score log differs from saved state"
        assert not any(entity["Name"].startswith("Block:") for entity in final_scene["Entities"]), \
            "Native row clear left locked block entities"
        print(f"Relocated Win32 game: real SendInput Space, two-row victory, score 336, "
              f"{result['frames']} validated frames, GDI before/after pixels and WM_CLOSE passed")


if __name__ == "__main__":
    main()
