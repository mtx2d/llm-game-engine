"""Exercise native Win32 editor authoring through real SendInput and client pixels."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from NativeEditorWorkflow import prepare_project, run_workflow, save_image
from WindowsRuntimeInputTests import (BOOL, HANDLE, GuiThreadInfo, Input, KeyboardInput, LONG, MouseInput,
                                     NativeWindow, Rectangle, wait_for_image)


class Point(ctypes.Structure):
    _fields_ = [("x", LONG), ("y", LONG)]


class EditorDriver(NativeWindow):
    def __init__(self, process, artifacts):
        super().__init__(process)
        self.user.ClientToScreen.argtypes = [HANDLE, ctypes.POINTER(Point)]
        self.user.ClientToScreen.restype = BOOL
        self.user.GetSystemMetrics.argtypes = [ctypes.c_int]
        self.user.GetSystemMetrics.restype = ctypes.c_int
        self.user.WindowFromPoint.argtypes = [Point]
        self.user.WindowFromPoint.restype = HANDLE
        self.user.GetCursorPos.argtypes = [ctypes.POINTER(Point)]
        self.user.GetCursorPos.restype = BOOL
        self.held_keys = set()
        self.held_mouse = False
        self.pointer_target = None
        self.artifacts = artifacts
        self.events = []
        self.started = time.monotonic()

    def record(self, operation, **values):
        assert len(self.events) < 1000, "Native input evidence exceeded its bound"
        self.events.append({"operation": operation, "seconds": time.monotonic() - self.started, **values})

    def cursor(self, verify_target=True):
        self.assert_alive()
        point = Point()
        self.check(self.user.GetCursorPos(ctypes.byref(point)), "GetCursorPos")
        hit_window = self.user.WindowFromPoint(point)
        matches = self.pointer_target is not None and abs(point.x - self.pointer_target.x) <= 1 and \
            abs(point.y - self.pointer_target.y) <= 1
        if hit_window != self.window or (verify_target and not matches):
            self.record("cursor rejected", cursor_screen=[point.x, point.y], hit_window=hit_window,
                        owned_window=self.window, verify_target=verify_target,
                        requested_screen=None if self.pointer_target is None else
                        [self.pointer_target.x, self.pointer_target.y])
        assert hit_window == self.window, "Actual cursor is outside the owned editor"
        assert not verify_target or matches, "Native cursor did not remain at the requested editor point"
        return [point.x, point.y]

    def capture_window(self):
        self.assert_alive()
        info = GuiThreadInfo()
        info.size = ctypes.sizeof(info)
        thread = self.owned_thread(self.window)
        self.check(self.user.GetGUIThreadInfo(thread, ctypes.byref(info)), "GetGUIThreadInfo")
        if info.capture not in (None, self.window):
            self.record("capture rejected", capture_window=info.capture, owned_window=self.window)
            raise AssertionError("Another window captured the editor's mouse input")
        return info.capture

    def wait_capture(self, pressed, timeout=10):
        deadline = time.monotonic() + timeout
        captured = self.capture_window()
        while time.monotonic() < deadline:
            captured = self.capture_window()
            if captured == (self.window if pressed else None):
                return captured
            time.sleep(0.02)
        self.record("capture timeout", pressed=pressed, capture_window=captured, owned_window=self.window)
        raise AssertionError("Editor did not acknowledge the native mouse capture transition")

    def initialize(self):
        self.find()
        self.focus()
        for key in (0x01, 0x02, 0x04, 0x10, 0x11, 0x12, 0x5B, 0x5C):
            assert not self.user.GetAsyncKeyState(key) & 0x8000, \
                f"Input 0x{key:02X} is already held; refusing to change existing state"
        wait_for_image(self, timeout=30)

    def assert_alive(self):
        self.require_alive()
        assert self.focused(), "Editor lost foreground keyboard focus; refusing input"

    def size(self):
        self.assert_alive()
        rectangle = Rectangle()
        self.check(self.user.GetClientRect(self.window, ctypes.byref(rectangle)), "GetClientRect")
        return rectangle.right - rectangle.left, rectangle.bottom - rectangle.top

    def settle(self):
        # Keep down/up events in separate native message-pump iterations. Saved
        # scene predicates in the common workflow establish action completion;
        # this pacing alone is not counted as a rendered-frame acknowledgment.
        time.sleep(0.25)
        self.assert_alive()

    def send(self, event):
        self.assert_alive()
        self.check(self.user.SendInput(1, ctypes.byref(event), ctypes.sizeof(Input)) == 1, "SendInput")

    def move(self, x, y):
        width, height = self.size()
        assert 0 <= x < width and 0 <= y < height, "Refusing pointer input outside the owned client"
        point = Point(round(x), round(y))
        self.check(self.user.ClientToScreen(self.window, ctypes.byref(point)), "ClientToScreen")
        assert self.user.WindowFromPoint(point) == self.window, "Pointer target is covered by another window"
        left, top = (self.user.GetSystemMetrics(index) for index in (76, 77))
        screen_width, screen_height = (self.user.GetSystemMetrics(index) for index in (78, 79))
        assert screen_width > 1 and screen_height > 1
        assert left <= point.x < left + screen_width and top <= point.y < top + screen_height
        event = Input()
        event.type = 0
        event.value.mouse = MouseInput(round((point.x - left) * 65535 / (screen_width - 1)),
                                       round((point.y - top) * 65535 / (screen_height - 1)),
                                       0, 0x0001 | 0x2000 | 0x4000 | 0x8000, 0, 0)
        # Preserve each WM_MOUSEMOVE. GLFW's Win32 button callback consumes the
        # position from earlier movement messages, not the button message itself.
        self.pointer_target = point
        self.record("move requested", client=[x, y], requested_screen=[point.x, point.y])
        self.send(event)
        self.settle()
        self.record("move", client=[x, y], requested_screen=[point.x, point.y], cursor_screen=self.cursor())

    def button(self, pressed, verify_target=True):
        assert self.held_mouse != pressed, "Unbalanced native mouse transition"
        self.record("button requested", pressed=pressed, verify_target=verify_target)
        self.cursor(verify_target)
        self.capture_window()
        event = Input()
        event.type = 0
        event.value.mouse = MouseInput(0, 0, 0, 0x0002 if pressed else 0x0004, 0, 0)
        self.send(event)
        self.held_mouse = pressed
        captured = self.wait_capture(pressed)
        self.settle()
        self.record("button", pressed=pressed, capture_window=captured, cursor_screen=self.cursor(verify_target))

    def key(self, code, pressed):
        assert (code in self.held_keys) != pressed, "Unbalanced native key transition"
        if pressed:
            assert not self.user.GetAsyncKeyState(code) & 0x8000, "Native test key was already held"
        event = Input()
        event.type = 1
        event.value.keyboard = KeyboardInput(code, 0, 0 if pressed else 0x0002, 0, 0)
        self.send(event)
        if pressed:
            self.held_keys.add(code)
        else:
            self.held_keys.remove(code)
        self.settle()

    def click(self, x, y):
        self.move(x, y)
        self.button(True)
        self.button(False)

    def drag(self, points):
        assert len(points) >= 2
        self.move(*points[0])
        # A fixed delay cannot prove ImGui has consumed the hover that makes an
        # ImGuizmo click eligible. Observe the actual highlighted world-X handle
        # before pressing, then each visible held-motion response before moving on.
        image = self.wait_drag_image("hover", lambda image: self.hover_pixels(image, points[0]), minimum=8)
        self.button(True)
        previous = image
        image = self.wait_drag_image("pressed", lambda image: self.changed_viewport_pixels(previous, image), minimum=32)
        for index, point in enumerate(points[1:], 1):
            previous = image
            self.move(*point)
            image = self.wait_drag_image(f"motion-{index}",
                                         lambda image: self.changed_viewport_pixels(previous, image), minimum=32)
        self.button(False)

    @staticmethod
    def hover_pixels(image, point):
        width, height, pixels = image
        x, y = map(round, point)
        assert 6 <= x < width - 6 and 6 <= y < height - 6
        # The pinned ImGuizmo selection color is orange, alpha blended over the
        # fixture. Neither the red unselected X axis nor the gray disabled axis
        # satisfies both channel differences in this small handle-only region.
        return sum(r > g + 30 and g > b + 30
                   for row in range(y - 6, y + 7) for column in range(x - 6, x + 7)
                   for r, g, b in [pixels[(row * width + column) * 3:(row * width + column) * 3 + 3]])

    @staticmethod
    def changed_viewport_pixels(before, after):
        width, height, pixels = before
        assert after[:2] == before[:2], "Native editor resized during the drag"
        current = after[2]
        # Exclude widgets, the toolbar and cursor sprites (GDI captures no cursor).
        return sum(sum(abs(a - b) for a, b in zip(pixels[offset:offset + 3], current[offset:offset + 3])) > 30
                   for row in range(130, height - 226) for column in range(270, width - 340)
                   for offset in [(row * width + column) * 3])

    def wait_drag_image(self, stage, measure, minimum, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.cursor()
            image = self.capture()
            pixels = measure(image)
            if pixels >= minimum:
                save_image(self.artifacts / f"Drag-{stage}.ppm", image)
                self.record("drag image", stage=stage, pixels=pixels, minimum=minimum)
                return image
            time.sleep(0.05)
        save_image(self.artifacts / f"Drag-{stage}-failed.ppm", image)
        self.record("drag image timeout", stage=stage, pixels=pixels, minimum=minimum)
        raise AssertionError(f"Native editor did not present the expected gizmo {stage} response")

    def replace_text(self, x, y, text):
        assert text.isascii() and len(text) <= 128, "Native editor test text is deliberately bounded ASCII"
        self.click(x, y)
        self.key(0x11, True)
        self.key(0x41, True)
        self.key(0x41, False)
        self.key(0x11, False)
        for character in text:
            # KEYEVENTF_UNICODE becomes actual WM_CHAR input through SendInput;
            # no text widget or engine callback is called directly.
            events = (Input * 2)()
            for index, flags in enumerate((0x0004, 0x0004 | 0x0002)):
                events[index].type = 1
                events[index].value.keyboard = KeyboardInput(0, ord(character), flags, 0, 0)
            self.assert_alive()
            sent = self.user.SendInput(2, events, ctypes.sizeof(Input))
            if sent == 1 and self.focused():
                self.check(self.user.SendInput(1, ctypes.byref(events[1]), ctypes.sizeof(Input)) == 1,
                           "release partially inserted Unicode input")
            self.check(sent == 2, "SendInput Unicode pair")
            self.assert_alive()
        self.settle()
        self.key(0x0D, True)
        self.key(0x0D, False)

    def close(self):
        assert not self.held_keys and not self.held_mouse, "Native workflow left input held"
        self.request_close()

    def cleanup(self):
        # Repair only events this driver inserted, and only while our owned
        # window still has focus. Never release another application's input.
        try:
            if self.process.poll() is None and self.focused():
                if self.held_mouse:
                    # The failed operation may have observed a moved cursor.
                    # Release our own button at its current owned-window point;
                    # foreground and hit-window checks still apply.
                    self.button(False, verify_target=False)
                for code in tuple(self.held_keys):
                    self.key(code, False)
        finally:
            super().close()


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "assets", "artifacts"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    assert os.name == "nt", "Windows editor interaction requires an interactive Windows desktop"
    editor, assets, artifacts = (getattr(args, name).resolve() for name in ("editor", "assets", "artifacts"))
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "Workflow.json").unlink(missing_ok=True)
    (artifacts / "EditorFinal.ppm").unlink(missing_ok=True)
    (artifacts / "Input.json").unlink(missing_ok=True)
    for previous in artifacts.glob("Drag-*.ppm"):
        previous.unlink()
    with tempfile.TemporaryDirectory(prefix="AsterWindowsEditor-") as temporary:
        project = Path(temporary) / "Assets"
        scene_path = prepare_project(assets, project)
        with (artifacts / "Editor.log").open("w") as log:
            process = subprocess.Popen([str(editor), "--project", str(project), "--audio", "offline",
                                        "--screenshot", str(artifacts / "EditorFinal.ppm")], stdout=log, stderr=log)
            driver = None
            try:
                driver = EditorDriver(process, artifacts)
                driver.initialize()
                report = run_workflow(driver, scene_path, artifacts)
                assert process.wait(timeout=30) == 0, "Native editor reported rendering or simulation errors"
                report["assertions"].append("graceful native close")
            finally:
                try:
                    if driver:
                        try:
                            driver.cleanup()
                        finally:
                            (artifacts / "Input.json").write_text(json.dumps(driver.events, indent=2) + "\n")
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)
            # Cleanup and evidence publication must succeed before claiming the
            # whole native workflow passed, including restoration of DPI state.
            (artifacts / "Workflow.json").write_text(json.dumps(report, indent=2) + "\n")
    print("Native Win32 editor: SendInput gizmo/text authoring, one-step Undo, save/reload, play/stop and WM_CLOSE passed")


if __name__ == "__main__":
    main()
