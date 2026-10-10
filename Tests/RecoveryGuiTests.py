"""Kill a real GUI editor, recover its automatic checkpoint and explicitly save/discard."""

import argparse
from pathlib import Path
import subprocess
import time

from GuiTests import close_window
from NativeWindow import WindowFrames, WindowRepaints
from NativeRecoveryWorkflow import run_recovery_workflow
from NativeProjectWorkflow import run_project_workflow


class Editor:
    def __init__(self, process, artifacts, diagnostics, size=None):
        self.process = process
        self.frames = self.pixels = None
        self.artifacts = artifacts
        self.events = []
        self.requested_size = size

    def initialize(self):
        self.window = self.wait(self.find_window, "Editor did not show its native window")
        self.xdo("windowfocus", self.window)
        if self.requested_size:
            self.xdo("windowsize", self.window, *self.requested_size)
            self.wait(lambda: all(f"{key}={value}" in self.xdo("getwindowgeometry", "--shell", self.window).stdout
                                  for key, value in zip(("WIDTH", "HEIGHT"), self.requested_size)),
                      "Native test window did not reach its requested size")
        geometry = dict(line.split("=", 1) for line in
                        self.xdo("getwindowgeometry", "--shell", self.window).stdout.splitlines())
        self.width, self.height = int(geometry["WIDTH"]), int(geometry["HEIGHT"])
        self.frames = WindowRepaints(self.window, self.width, self.height)
        self.pixels = WindowFrames(self.window, self.width, self.height)
        self.frames.wait(self.process, timeout=30)

    def size(self):
        return self.width, self.height

    def assert_alive(self):
        assert self.process.poll() is None, "Editor exited during recovery workflow"

    def capture(self):
        rgb = self.pixels.sample_rgb((x, y) for y in range(self.height) for x in range(self.width))
        return self.width, self.height, bytes(channel for pixel in rgb for channel in pixel)

    def close(self):
        close_window(self.window)

    def xdo(self, *arguments, check=True):
        return subprocess.run(["xdotool", *map(str, arguments)], check=check,
                              capture_output=True, text=True, timeout=5)

    def find_window(self):
        result = self.xdo("search", "--onlyvisible", "--pid", self.process.pid, check=False)
        return result.stdout.splitlines()[0] if result.returncode == 0 and result.stdout.strip() else None

    def wait(self, predicate, message, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            assert self.process.poll() is None, "Editor exited during recovery workflow"
            result = predicate()
            if result:
                return result
            time.sleep(0.05)
        raise AssertionError(message)

    def input(self, *arguments):
        self.frames.drain()
        self.xdo(*arguments)
        self.frames.wait(self.process)

    def click(self, x, y):
        self.input("mousemove", "--window", self.window, x, y)
        self.input("mousedown", 1)
        self.input("mouseup", 1)

    def replace_text(self, x, y, text):
        self.click(x, y)
        for operation, key in (("keydown", "Control_L"), ("keydown", "a"),
                               ("keyup", "a"), ("keyup", "Control_L")):
            self.input(operation, key)
        self.input("type", "--clearmodifiers", "--delay", 20, text)
        self.input("key", "Return")

    def cleanup(self):
        for reader in (self.frames, self.pixels):
            if reader:
                reader.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", nargs=2, type=int)
    parser.add_argument("--projects-only", action="store_true")
    for field in ("editor", "assets", "artifacts"):
        parser.add_argument("--" + field, type=Path, required=True)
    args = parser.parse_args()
    args.editor, args.assets, args.artifacts = (getattr(args, field).resolve()
                                              for field in ("editor", "assets", "artifacts"))
    args.artifacts.mkdir(parents=True, exist_ok=True)
    workflow = run_project_workflow if args.projects_only else run_recovery_workflow
    workflow(args.editor, args.assets, args.artifacts,
             lambda process, evidence, diagnostics: Editor(process, evidence, diagnostics, args.size))


if __name__ == "__main__":
    main()
