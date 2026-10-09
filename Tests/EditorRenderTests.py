"""Check the native editor's GPU output, play mode, and teardown diagnostics."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


def read_image(path):
    with path.open("rb") as stream:
        assert stream.readline() == b"P6\n", "Editor did not write an RGB image"
        width, height = map(int, stream.readline().split())
        assert stream.readline() == b"255\n"
        # Native displays may constrain the requested window or apply Retina/DPI
        # scaling. Validate the actual framebuffer without assuming that scale.
        assert 640 <= width <= 8192 and 480 <= height <= 8192 and width * height <= 16 * 1024 * 1024, \
            "Unexpected editor framebuffer size"
        pixels = stream.read()
    assert len(pixels) == width * height * 3, "Editor readback has incomplete pixels"
    return width, height, pixels


def region(image, rectangle):
    width, height, pixels = image
    left, top, right, bottom = rectangle
    left, right = round(left * width / 1440), round(right * width / 1440)
    top, bottom = round(top * height / 900), round(bottom * height / 900)
    return b"".join(pixels[(row * width + left) * 3:(row * width + right) * 3]
                    for row in range(top, bottom))


def sample_region(image, rectangle, columns=64, rows=40):
    """Compare a fixed grid even when the desktop constrains two window sizes differently."""
    width, height, pixels = image
    left, top, right, bottom = rectangle
    samples = []
    for row in range(rows):
        y = int((top + (row + 0.5) * (bottom - top) / rows) * height / 900)
        for column in range(columns):
            x = int((left + (column + 0.5) * (right - left) / columns) * width / 1440)
            offset = (y * width + x) * 3
            samples.append(tuple(pixels[offset:offset + 3]))
    return samples


def main():
    parser = argparse.ArgumentParser()
    for name in ("editor", "assets", "artifacts"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    editor, assets, artifacts = (getattr(args, name).resolve() for name in ("editor", "assets", "artifacts"))
    artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="AsterEditorRender-") as temporary:
        project = Path(temporary) / "Assets"
        shutil.copytree(assets, project)
        scene_path = project / "Scenes/FeatureGallery.aster"
        original_scene = scene_path.read_bytes()

        def launch(name, playing, expected_error=None):
            screenshot = artifacts / f"{name}.ppm"
            screenshot.unlink(missing_ok=True)
            command = [str(editor), "--project", str(project), "--audio", "offline",
                       "--frames", "5", "--screenshot", str(screenshot)]
            if playing:
                command.append("--play")
            process = subprocess.run(command, text=True, capture_output=True, timeout=90)
            (artifacts / f"{name}.log").write_text(process.stdout + process.stderr, encoding="utf-8")
            if expected_error is None:
                assert process.returncode == 0, process.stdout + process.stderr
            else:
                assert process.returncode == 1 and expected_error in process.stderr, \
                    f"Editor lost teardown failure: {process.returncode}\n{process.stdout}\n{process.stderr}"
            # Even the failure case must finish rendering before OnDestroy fails.
            return read_image(screenshot)

        views = []
        for name, playing in (("EditorAuthored", False), ("EditorPlaying", True)):
            image = launch(name, playing)
            viewport = region(image, (270, 115, 1090, 600))
            colors = set(zip(viewport[0::3], viewport[1::3], viewport[2::3]))
            assert len(colors) > 500, f"{name}: scene viewport lacks actual shaded GPU output"
            sidebar = region(image, (10, 120, 245, 570))
            bright_text = sum(all(channel > 170 for channel in pixel)
                              for pixel in zip(sidebar[0::3], sidebar[1::3], sidebar[2::3]))
            assert bright_text > 100, f"{name}: native GUI hierarchy text was not rendered"
            assert scene_path.read_bytes() == original_scene, "Editor play overwrote the authored scene"
            views.append(sample_region(image, (270, 115, 1090, 600)))
        # Cocoa can constrain successive requested 1440x900 windows to slightly
        # different framebuffers (observed 1024x653 and 1024x656). Require a
        # substantive visual difference, not mismatched dimensions or rounding.
        changed = sum(sum(abs(a - b) for a, b in zip(first, second)) > 30
                      for first, second in zip(*views))
        assert changed > len(views[0]) * 0.05, \
            f"Editor play view lacks distinct rendered content: {changed}/{len(views[0])} samples changed"

        scene = json.loads(original_scene)
        for entity in scene["Entities"]:
            entity.pop("Script", None)
        scene["Entities"][0]["Script"] = {"Path": "Scripts/EditorTeardown.lua", "Enabled": True}
        (project / "Scripts/EditorTeardown.lua").write_text(
            'return {OnDestroy=function(self, entity) error("editor window teardown sentinel") end}',
            encoding="utf-8",
        )
        scene_path.write_text(json.dumps(scene), encoding="utf-8")
        launch("EditorTeardown", True, "editor window teardown sentinel")
    print("Native editor authored/play GPU pixels, hierarchy text, preserved scene and teardown errors passed")


if __name__ == "__main__":
    main()
