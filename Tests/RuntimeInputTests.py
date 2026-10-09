"""Drive a relocated shipping game through real X11 keyboard input and presentation."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from BlockStackTests import state
from ExportTests import export
from GuiTests import close_window
from NativeWindow import WindowFrames


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    if not os.environ.get("DISPLAY") or not shutil.which("xdotool"):
        raise RuntimeError("Native runtime input requires DISPLAY and xdotool (use xvfb-run)")
    editor, runtime, assets, notices = (path.resolve() for path in
                                        (args.editor, args.runtime, args.assets, args.notices))
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    output = artifacts / "RuntimeInput.aster"
    output.unlink(missing_ok=True)
    stdout_path = artifacts / "RuntimeInput.json"
    stderr_path = artifacts / "RuntimeInput.stderr"

    def xdo(*arguments, check=True):
        return subprocess.run(["xdotool", *map(str, arguments)], check=check,
                              capture_output=True, text=True, timeout=5)

    with tempfile.TemporaryDirectory(prefix="AsterNativeInput-") as temporary:
        root = Path(temporary)
        source = root / "Source Assets"
        shutil.copytree(assets, source)
        package = root / "Exported Game"
        response = export(editor, runtime, notices, source, package,
                          scene="Games/BlockStack/LineClear.aster")
        assert response.get("ok"), f"Native input fixture export failed: {response}"
        moved = root / "Relocated Game With Spaces"
        package.rename(moved)
        shutil.rmtree(source)
        elsewhere = root / "Unrelated Working Directory"
        elsewhere.mkdir()
        assert not source.exists() and not package.exists(), "Export fixture still has its source paths"
        executable = moved / "AsterGame"
        with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
            process = subprocess.Popen([str(executable), "--window", "--steps", "10000",
                                        "--validation", "--output", str(output)],
                                       cwd=elsewhere, stdout=stdout, stderr=stderr)
            frames = None
            key_held = False
            try:
                deadline = time.monotonic() + 30
                window = None
                while time.monotonic() < deadline:
                    assert process.poll() is None, "Exported runtime exited before showing its window"
                    result = xdo("search", "--onlyvisible", "--pid", process.pid, check=False)
                    if result.returncode == 0 and result.stdout.strip():
                        window = result.stdout.splitlines()[0]
                        break
                    time.sleep(0.02)
                assert window, "Relocated game did not show a visible native window"
                xdo("windowfocus", window)
                geometry = dict(line.split("=", 1) for line in
                                xdo("getwindowgeometry", "--shell", window).stdout.splitlines())
                frames = WindowFrames(window, int(geometry["WIDTH"]), int(geometry["HEIGHT"]))
                first_frame = None
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    assert process.poll() is None, "Exported runtime exited before presenting its scene"
                    fingerprint, populated = frames.capture()
                    if populated:
                        first_frame = fingerprint
                        break
                    time.sleep(0.02)
                assert first_frame, "Relocated game did not present a populated scene image"

                # Keep Space held until another image is presented. This spans a
                # GLFW poll even if the first GPU frame or shader setup is slow.
                xdo("keydown", "--clearmodifiers", "space")
                key_held = True
                deadline = time.monotonic() + 10
                changed = False
                while time.monotonic() < deadline:
                    assert process.poll() is None, "Exported runtime exited during native key input"
                    fingerprint, populated = frames.capture()
                    if populated and fingerprint != first_frame:
                        changed = True
                        break
                    time.sleep(0.02)
                assert changed, "Space input did not lead to another presented game image"
                xdo("keyup", "space")
                key_held = False
                close_window(window)
                assert process.wait(timeout=20) == 0, "Native runtime reported a gameplay or Vulkan validation error"
            finally:
                if key_held:
                    xdo("keyup", "space", check=False)
                if frames:
                    frames.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                stdout.flush()
                stderr.flush()
                diagnostics = stderr_path.read_text()
                if diagnostics:
                    print(diagnostics)

        result = json.loads(stdout_path.read_text())
        assert result["errors"] == [], f"Native exported gameplay script errors: {result}"
        assert result["frames"] == result["steps"] and 2 <= result["frames"] < 10000, \
            "Native runtime did not render frames and exit through the window close protocol"
        # Gravity alone needs over 700 fixed updates to clear the seeded rows.
        # This bound proves the observed victory came from the keyboard hard drop.
        assert result["steps"] < 600, "Test ran long enough for gravity alone to cause victory"
        assert "BlockStack: ready" in result["log"] and "BlockStack: victory" in result["log"], \
            "Relocated game did not initialize and reach its authored victory transition"
        final_scene = json.loads(output.read_text())
        victory = state(final_scene)
        assert victory["status"] == "won" and victory["lines"] == 2, \
            f"Real Space input did not clear the two seeded rows: {victory}"
        # Each natural fall before the key arrives can reduce the drop score by
        # two; two cleared rows still award exactly 300 points.
        assert 300 < victory["score"] <= 336 and (victory["score"] - 300) % 2 == 0, \
            f"Native hard drop did not score distance plus two cleared rows: {victory}"
        assert f"BlockStack: cleared 2 rows; score={victory['score']}" in result["log"], \
            "Saved score does not match the gameplay scoring event"
        assert not any(entity["Name"].startswith("Block:") for entity in final_scene["Entities"]), \
            "Native row clear retained locked block entities"
        print(f"Relocated native game: real Space input, two-row victory, score {victory['score']}, "
              f"{result['frames']} validated frames and normal window shutdown passed")


if __name__ == "__main__":
    main()
