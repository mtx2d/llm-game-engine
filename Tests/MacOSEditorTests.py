"""Real macOS pointer/keyboard editor authoring with owned-window capture."""

import argparse
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time

from MacOSNativeWindow import compile_helper
from NativeEditorWorkflow import prepare_project, run_workflow
from NativeRecoveryWorkflow import run_recovery_workflow
from NativeProjectWorkflow import run_project_workflow


def check_process_identity(helper, artifacts):
    # Exercise the real kernel query without input permissions, a GUI or an
    # AppKit registry entry. An exited/replaced target must never pass it.
    victim = subprocess.Popen(["/bin/sleep", "60"])
    observations = []
    try:
        for exited in (False, True):
            with subprocess.Popen([str(helper), "--check-process", str(victim.pid), "/bin/sleep"],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as probe:
                try:
                    with selectors.DefaultSelector() as selector:
                        selector.register(probe.stdout, selectors.EVENT_READ)
                        assert selector.select(10), "Kernel identity probe did not become ready"
                    assert json.loads(probe.stdout.readline()).get("identity_ready")
                    if exited:
                        victim.terminate()
                        victim.wait(timeout=5)
                    output, errors = probe.communicate(b"check\n", timeout=10)
                    if exited:
                        assert probe.returncode != 0 and not output and errors, "Exited target passed identity check"
                    else:
                        assert probe.returncode == 0 and json.loads(output).get("identity_checked"), errors
                    observations.append({"exited": exited, "returncode": probe.returncode,
                                         "diagnostic": errors.decode()})
                finally:
                    if probe.poll() is None:
                        probe.kill()
                        probe.wait(timeout=5)
        with subprocess.Popen(["/bin/sleep", "60"]) as wrong_target:
            try:
                wrong = subprocess.run([str(helper), "--check-process", str(wrong_target.pid), "/bin/ls"],
                                       input=b"check\n", capture_output=True, timeout=10)
                assert wrong.returncode != 0 and b"does not belong" in wrong.stderr and not wrong.stdout
                observations.append({"wrong_executable": True, "returncode": wrong.returncode,
                                     "diagnostic": wrong.stderr.decode()})
            finally:
                wrong_target.terminate()
                wrong_target.wait(timeout=5)
    finally:
        if victim.poll() is None:
            victim.kill()
            victim.wait(timeout=5)
    (artifacts / "ProcessIdentity.json").write_text(json.dumps(observations, indent=2) + "\n")


class MacEditorDriver:
    def __init__(self, editor, helper, executable, artifacts, diagnostics):
        self.editor = editor
        self.artifacts = artifacts
        self.closed = False
        self.pending = b""
        self.events = []
        self.helper = subprocess.Popen([str(helper), str(editor.pid), str(executable), str(artifacts)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=diagnostics)

    def initialize(self):
        # The owner assigns this driver before waiting, so failed readiness also
        # closes the helper and its owned capture/input resources.
        self.ready = self.read_response(timeout=65)
        assert self.ready["pid"] == self.editor.pid and self.ready["window_id"] > 0
        assert all(self.ready["permissions"].get(name) is True
                   for name in ("accessibility", "post_event", "screen_capture"))
        assert 900 <= self.ready["width"] <= 8192 and 600 <= self.ready["height"] <= 8192

    def read_response(self, timeout=30, allow_exit=False):
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(self.helper.stdout, selectors.EVENT_READ)
            while b"\n" not in self.pending:
                if not allow_exit:
                    assert self.editor.poll() is None, "Editor exited during native authoring"
                assert selector.select(max(0, deadline - time.monotonic())), \
                    "Native macOS editor helper did not acknowledge its bounded input operation"
                chunk = os.read(self.helper.stdout.fileno(), 65536)
                assert chunk, "Native helper exited; inspect MacOSEditorInput.stderr"
                self.pending += chunk
                assert len(self.pending) <= 65536, "Native editor response exceeded its bound"
        line, self.pending = self.pending.split(b"\n", 1)
        response = json.loads(line)
        assert response.get("ok"), response
        self.events.append(response)
        return response

    def request(self, operation, **arguments):
        self.assert_alive()
        self.helper.stdin.write(json.dumps({"operation": operation, **arguments}).encode() + b"\n")
        self.helper.stdin.flush()
        # The AX close action may finish either process before Python receives
        # the final buffered acknowledgment. Read it, then check both exit codes.
        return self.read_response(allow_exit=operation == "close")

    def assert_alive(self):
        assert self.editor.poll() is None, "Editor exited during native authoring"
        assert self.helper.poll() is None, "Native helper exited; inspect MacOSEditorInput.stderr"

    def size(self):
        return self.ready["width"], self.ready["height"]

    def mouse(self, action, point):
        self.request("mouse", action=action, x=float(point[0]), y=float(point[1]))

    def click(self, x, y):
        point = (x, y)
        self.mouse("move", point)
        self.mouse("down", point)
        self.mouse("up", point)

    def drag(self, points):
        assert len(points) >= 2
        self.mouse("move", points[0])
        self.mouse("down", points[0])
        for point in points[1:]:
            self.mouse("move", point)
        self.mouse("up", points[-1])

    def replace_text(self, x, y, text):
        self.click(x, y)
        # The helper paces modifier/key edges so ImGui can observe Command+A.
        # Persisted scene predicates establish completion after text input.
        for key, down in (("Command", True), ("A", True), ("A", False), ("Command", False)):
            self.request("key", key=key, down=down)
        self.request("text", text=text)
        self.request("key", key="Return", down=True)
        self.request("key", key="Return", down=False)

    def capture(self):
        self.request("capture")
        with (self.artifacts / "EditorSnapshot.ppm").open("rb") as image:
            assert image.readline() == b"P6\n"
            width, height = map(int, image.readline().split())
            assert image.readline() == b"255\n"
            assert (width, height) == self.size(), "Capture/client coordinates disagree"
            pixels = image.read(width * height * 3 + 1)
        assert len(pixels) == width * height * 3
        return width, height, pixels

    def request_close(self):
        response = self.request("requestClose")
        assert response.get("requested"), "Native helper did not request close"

    def detach(self):
        response = self.request("detach")
        assert response.get("detached"), "Native helper did not detach before the intentional editor crash"
        assert self.helper.wait(timeout=10) == 0, "Native helper failed while detaching"

    def close(self):
        if not self.closed:
            response = self.request("close")
            assert response.get("closed"), "Native helper did not press its owned window's close button"
            self.closed = True
            assert self.helper.wait(timeout=10) == 0, "Native helper failed during graceful close"

    def cleanup(self):
        (self.artifacts / "MacOSEditorEvents.json").write_text(json.dumps(self.events, indent=2) + "\n")
        if self.helper.poll() is None:
            # EOF lets Swift release only its own held controls.
            self.helper.stdin.close()
            try:
                self.helper.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.helper.terminate()
                try:
                    self.helper.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.helper.kill()
                    self.helper.wait(timeout=5)
        for stream in (self.helper.stdin, self.helper.stdout):
            stream.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recovery-only", action="store_true")
    parser.add_argument("--projects-only", action="store_true")
    for name in ("editor", "assets", "artifacts"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    editor, assets, artifacts = (getattr(args, name).resolve() for name in ("editor", "assets", "artifacts"))
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "Workflow.json").unlink(missing_ok=True)
    helper, _ = compile_helper("MacOSEditorInput", artifacts)
    if args.recovery_only or args.projects_only:
        workflow = run_project_workflow if args.projects_only else run_recovery_workflow
        workflow(editor, assets, artifacts,
                 lambda process, evidence, diagnostics:
                 MacEditorDriver(process, helper, editor, evidence, diagnostics))
        return
    check_process_identity(helper, artifacts)
    with tempfile.TemporaryDirectory(prefix="AsterMacOSEditor-") as temporary:
        project = Path(temporary) / "Assets"
        scene = prepare_project(assets, project)
        with (artifacts / "Editor.log").open("w") as log, \
             (artifacts / "MacOSEditorInput.stderr").open("w") as diagnostics:
            process = subprocess.Popen([str(editor), "--project", str(project), "--audio", "offline",
                                        "--screenshot", str(artifacts / "EditorFinal.ppm")], stdout=log, stderr=log)
            driver = None
            try:
                driver = MacEditorDriver(process, helper, editor, artifacts, diagnostics)
                driver.initialize()
                report = run_workflow(driver, scene, artifacts)
                assert process.wait(timeout=15) == 0, "Editor failed after native close"
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
                    log.flush()
                    diagnostics.flush()
                    if process.returncode:
                        print((artifacts / "Editor.log").read_text())
                    if (artifacts / "MacOSEditorInput.stderr").stat().st_size:
                        print((artifacts / "MacOSEditorInput.stderr").read_text())
    print("macOS native editor authoring, coalesced gizmo Undo, save/reload, play/stop and graceful close passed")


if __name__ == "__main__":
    main()
