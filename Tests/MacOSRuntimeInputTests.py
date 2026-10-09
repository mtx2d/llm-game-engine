"""Run relocated gameplay through Quartz input and an owned ScreenCaptureKit window.

Requires macOS 14+, Xcode command-line tools, an interactive desktop, and existing
Accessibility/PostEvent/ScreenCapture grants. The helper checks public preflight
APIs without requesting permission or changing TCC. Missing access fails the test.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from BlockStackTests import state
from MacOSNativeWindow import compile_helper
from ExportTests import export, macos_shipping_environment, package_executable


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "runtime", "assets", "notices", "artifacts"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    editor, runtime, assets, notices, artifacts = (getattr(args, name).resolve()
        for name in ("editor", "runtime", "assets", "notices", "artifacts"))
    helper, permissions = compile_helper("MacOSRuntimeInput", artifacts)

    output = artifacts / "MacOSRuntimeInput.aster"
    stdout_path = artifacts / "MacOSRuntimeInput.json"
    stderr_path = artifacts / "MacOSRuntimeInput.stderr"
    for path in (output, artifacts / "MacOSRuntimeInputBefore.ppm", artifacts / "MacOSRuntimeInputAfter.ppm"):
        path.unlink(missing_ok=True)
    with tempfile.TemporaryDirectory(prefix="AsterMacOSInput-") as temporary:
        root = Path(temporary)
        source, package = root / "Source Assets", root / "Exported Game"
        shutil.copytree(assets, source)
        response = export(editor, runtime, notices, source, package, scene="Games/BlockStack/LineClear.aster")
        assert response.get("ok"), f"Native macOS input fixture export failed: {response}"
        moved = root / "Relocated Game With Spaces"
        package.rename(moved)
        shutil.rmtree(source)
        elsewhere = root / "Unrelated Working Directory"
        elsewhere.mkdir()
        assert not source.exists() and not package.exists(), "Source paths survived relocation"
        executable = package_executable(moved)
        with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
            # The first run proves bundled driver/library discovery without SDK
            # settings. GraphicalExport separately covers explicit validation layers.
            process = subprocess.Popen([str(executable), "--window", "--steps", "10000", "--output", str(output)],
                                       cwd=elsewhere, env=macos_shipping_environment(), stdout=stdout, stderr=stderr)
            try:
                interaction = subprocess.run([str(helper), str(process.pid), str(executable), str(artifacts)],
                                             cwd=elsewhere, capture_output=True, text=True, timeout=90)
                (artifacts / "MacOSRuntimeInputInteraction.json").write_text(interaction.stdout)
                (artifacts / "MacOSRuntimeInputInteraction.stderr").write_text(interaction.stderr)
                assert interaction.returncode == 0, \
                    f"Native Quartz/window interaction failed: {interaction.stdout}\n{interaction.stderr}"
                native = json.loads(interaction.stdout)
                assert native["pid"] == process.pid and native["window_id"] > 0 and native["close_requested"], \
                    f"Native interaction did not use the launched process/window: {native}"
                assert native["before_sha256"] != native["after_sha256"], "Native game pixels did not change"
                assert all(native["permissions"].get(name) is True for name in permissions), \
                    "Native interaction process lacked a required permission"
                assert process.wait(timeout=30) == 0, "Native game did not exit normally after its close-button action"
            finally:
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
        assert result["errors"] == [], f"Native exported gameplay errors: {result}"
        assert result["frames"] == result["steps"] and 2 <= result["frames"] < 600, \
            "Runtime must present frames and close before gravity alone can win"
        assert "BlockStack: ready" in result["log"] and "BlockStack: victory" in result["log"], \
            "Relocated game did not initialize and reach victory"
        final_scene = json.loads(output.read_text())
        victory = state(final_scene)
        assert victory["status"] == "won" and victory["lines"] == 2, \
            f"Quartz Space did not produce the authored two-row victory: {victory}"
        # Native capture/focus can span a natural fall, reducing the hard-drop
        # distance bonus by two. Gravity alone awards only the 300 row points.
        assert 300 < victory["score"] <= 336 and (victory["score"] - 300) % 2 == 0, \
            f"Native hard-drop distance did not contribute the expected bonus: {victory}"
        assert f"BlockStack: cleared 2 rows; score={victory['score']}" in result["log"], \
            "Saved score differs from scoring event"
        assert not any(entity["Name"].startswith("Block:") for entity in final_scene["Entities"]), \
            "Native row clear retained locked blocks"
        for phase in ("Before", "After"):
            with (artifacts / f"MacOSRuntimeInput{phase}.ppm").open("rb") as image:
                assert image.readline() == b"P6\n"
                width, height = map(int, image.readline().split())
                assert image.readline() == b"255\n"
                pixels = image.read()
                assert 0 < width * height <= 16 * 1024 * 1024 and len(pixels) == width * height * 3
                assert len(set(pixels)) > 32, "Native screenshot lacks populated scene pixels"
        print(f"Relocated macOS game: real PID-directed Quartz Space, two-row victory, score {victory['score']}, "
              f"{result['frames']} frames, owned-window pixels and graceful close passed")


if __name__ == "__main__":
    main()
