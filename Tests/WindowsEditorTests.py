"""Exercise native Win32 editor authoring through real SendInput and client pixels."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from NativeEditorWorkflow import prepare_project, run_workflow
from WindowsRuntimeInputTests import (BOOL, HANDLE, Input, KeyboardInput, LONG, MouseInput,
                                     NativeWindow, Rectangle, wait_for_image)


class Point(ctypes.Structure):
    _fields_ = [("x", LONG), ("y", LONG)]


class EditorDriver(NativeWindow):
    def __init__(self, process):
        super().__init__(process)
        self.user.ClientToScreen.argtypes = [HANDLE, ctypes.POINTER(Point)]
        self.user.ClientToScreen.restype = BOOL
        self.user.GetSystemMetrics.argtypes = [ctypes.c_int]
        self.user.GetSystemMetrics.restype = ctypes.c_int
        self.user.WindowFromPoint.argtypes = [Point]
        self.user.WindowFromPoint.restype = HANDLE
        self.held_keys = set()
        self.held_mouse = False

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
                                       0, 0x0001 | 0x4000 | 0x8000, 0, 0)
        self.send(event)
        self.settle()

    def button(self, pressed):
        assert self.held_mouse != pressed, "Unbalanced native mouse transition"
        event = Input()
        event.type = 0
        event.value.mouse = MouseInput(0, 0, 0, 0x0002 if pressed else 0x0004, 0, 0)
        self.send(event)
        self.held_mouse = pressed
        self.settle()

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
        self.button(True)
        for point in points[1:]:
            self.move(*point)
        self.button(False)

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
                    self.button(False)
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
    with tempfile.TemporaryDirectory(prefix="AsterWindowsEditor-") as temporary:
        project = Path(temporary) / "Assets"
        scene_path = prepare_project(assets, project)
        with (artifacts / "Editor.log").open("w") as log:
            process = subprocess.Popen([str(editor), "--project", str(project), "--audio", "offline",
                                        "--screenshot", str(artifacts / "EditorFinal.ppm")], stdout=log, stderr=log)
            driver = None
            try:
                driver = EditorDriver(process)
                driver.initialize()
                report = run_workflow(driver, scene_path, artifacts)
                assert process.wait(timeout=30) == 0, "Native editor reported rendering or simulation errors"
                report["assertions"].append("graceful native close")
                (artifacts / "Workflow.json").write_text(json.dumps(report, indent=2) + "\n")
            finally:
                try:
                    if driver:
                        driver.cleanup()
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)
    print("Native Win32 editor: SendInput gizmo/text authoring, one-step Undo, save/reload, play/stop and WM_CLOSE passed")


if __name__ == "__main__":
    main()
