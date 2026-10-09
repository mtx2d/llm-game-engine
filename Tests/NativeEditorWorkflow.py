"""Shared scene assertions for native Windows/macOS editor authoring adapters.

Drivers supply real owned-window input in logical client coordinates. This module
never sends editor automation commands or edits scene files after launching the GUI.
"""

import json
import math
from pathlib import Path
import shutil
import struct
import time

from GuiTests import project_editor_point


def prepare_project(assets, project):
    shutil.copytree(assets, project)
    return Path(project) / "Scenes/FeatureGallery.aster"


def save_image(path, image):
    width, height, pixels = image
    assert 640 <= width <= 8192 and 480 <= height <= 8192 and width * height <= 16 * 1024 * 1024
    assert len(pixels) == width * height * 3 and len(set(pixels)) > 32, "Native editor image lacks rendered pixels"
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def viewport_samples(image, logical_size, columns=64, rows=40):
    pixel_width, pixel_height, pixels = image
    width, height = logical_size
    # Exclude side panels, the top toolbar/gizmo buttons and the bottom asset
    # browser. Convert client coordinates explicitly for Retina/DPI captures.
    left, top, right, bottom = 270, 130, width - 340, height - 226
    assert left < right and top < bottom, "Native client has no usable scene viewport"
    samples = []
    for row in range(rows):
        y = int((top + (row + 0.5) * (bottom - top) / rows) * pixel_height / height)
        for column in range(columns):
            x = int((left + (column + 0.5) * (right - left) / columns) * pixel_width / width)
            offset = (y * pixel_width + x) * 3
            samples.append(tuple(pixels[offset:offset + 3]))
    return samples


def run_workflow(driver, scene_path, artifacts):
    scene_path, artifacts = Path(scene_path), Path(artifacts)
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "Workflow.json").unlink(missing_ok=True)
    # Authored decimal component values become C++ floats when loaded. Match
    # their exact binary32 values so a legitimate first Save still equals the
    # complete fixture, without broadly tolerating unrelated scene mutations.
    baseline = json.loads(scene_path.read_text(),
                          parse_float=lambda value: struct.unpack("<f", struct.pack("<f", float(value)))[0])
    gallery = next(entity for entity in baseline["Entities"] if entity["Name"] == "Gallery")
    original_transform = gallery["Transform"]
    gallery_id = gallery["ID"]
    width, height = driver.size()
    assert width >= 900 and height >= 600, "Native desktop leaves insufficient room for the editor workflow"

    def entity(scene, identifier=gallery_id):
        return next(value for value in scene["Entities"] if value["ID"] == identifier)

    def save_and_wait(predicate, timeout=15):
        deadline = time.monotonic() + timeout
        previous_write = scene_path.stat().st_mtime_ns
        while time.monotonic() < deadline:
            driver.assert_alive()
            driver.click(702, 21)
            try:
                saved = json.loads(scene_path.read_text())
                if scene_path.stat().st_mtime_ns != previous_write and predicate(saved):
                    return saved
            except (OSError, json.JSONDecodeError):
                pass
            time.sleep(0.1)
        (artifacts / "LastSaved.aster").write_bytes(scene_path.read_bytes())
        save_image(artifacts / "EditorFailure.ppm", driver.capture())
        raise AssertionError("Native editor did not persist the expected state; see LastSaved.aster")

    # A newly visible native window can still expose desktop pixels before its
    # first presentation. Prove that the actual editor handled a Save click and
    # wrote the unchanged fixture before sending the first authoring gesture.
    save_and_wait(lambda scene: scene == baseline, timeout=30)
    save_image(artifacts / "EditorBefore.ppm", driver.capture())

    # Select the actual Gallery entity, then drag its world-X handle across eight
    # separate motion events. ImGuizmo's screen-space handle scales with width.
    driver.click(55, 151)
    position = original_transform["Translation"]
    center = project_editor_point(position, width, height)
    endpoint = project_editor_point((position[0] + 1, position[1], position[2]), width, height)
    delta = tuple(end - start for start, end in zip(center, endpoint))
    length = math.hypot(*delta)
    direction = tuple(value / length for value in delta)
    start_distance, travel = 45 * width / 1440, 55 * width / 1440
    path = [tuple(value + axis * (start_distance + travel * step / 8)
                  for value, axis in zip(center, direction)) for step in range(9)]
    assert all(260 < x < width - 330 and 74 < y < height - 216 for x, y in path), \
        "Projected gizmo path crosses an editor panel"
    driver.drag(path)
    moved = save_and_wait(lambda scene: entity(scene)["Transform"]["Translation"][0] > position[0] + 0.25)
    transform = entity(moved)["Transform"]
    assert all(abs(transform["Translation"][axis] - position[axis]) < 1e-5 for axis in (1, 2)), \
        "World-X gizmo changed a different position axis"
    assert transform["Rotation"] == original_transform["Rotation"] and transform["Scale"] == original_transform["Scale"], \
        "Translation gizmo changed orientation or scale"
    save_image(artifacts / "EditorMoved.ppm", driver.capture())
    driver.click(147, 21)
    save_and_wait(lambda scene: entity(scene)["Transform"] == original_transform)

    # Actual text input, one Undo/Redo, and Load must pass through GUI commands.
    driver.replace_text(width - 250, 221, "NativeEditedGallery")
    save_and_wait(lambda scene: entity(scene)["Name"] == "NativeEditedGallery")
    driver.click(147, 21)
    save_and_wait(lambda scene: entity(scene)["Name"] == "Gallery")
    driver.click(190, 21)
    saved = save_and_wait(lambda scene: entity(scene)["Name"] == "NativeEditedGallery")
    before_unsaved = scene_path.read_bytes()
    driver.replace_text(width - 250, 221, "UnsavedNativeEdit")
    assert scene_path.read_bytes() == before_unsaved, "Inspector unexpectedly auto-saved the project"
    driver.click(658, 21)
    save_and_wait(lambda scene: scene == saved)

    driver.click(55, 114)
    created = save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]) + 1)
    added = [value for value in created["Entities"] if value["ID"] not in {item["ID"] for item in baseline["Entities"]}]
    assert len(added) == 1 and added[0]["Name"] == "Entity", "Create entity did not add exactly one default entity"
    created_id = added[0]["ID"]
    driver.replace_text(width - 250, 221, "NativeCreatedEntity")
    authored = save_and_wait(lambda scene: entity(scene, created_id)["Name"] == "NativeCreatedEntity")
    authored_bytes, authored_time = scene_path.read_bytes(), scene_path.stat().st_mtime_ns
    before_play = driver.capture()
    save_image(artifacts / "EditorAuthored.ppm", before_play)
    driver.click(232, 21)
    time.sleep(0.5)
    playing = driver.capture()
    save_image(artifacts / "EditorPlaying.ppm", playing)
    authored_samples = viewport_samples(before_play, (width, height))
    playing_samples = viewport_samples(playing, (width, height))
    changed = sum(sum(abs(a - b) for a, b in zip(first, second)) > 30
                  for first, second in zip(authored_samples, playing_samples))
    assert changed > len(authored_samples) * 0.05, \
        f"Play did not substantially change the scene viewport: {changed}/{len(authored_samples)} samples"
    driver.click(702, 21)
    assert scene_path.read_bytes() == authored_bytes and scene_path.stat().st_mtime_ns == authored_time, \
        "Play mode allowed Save to overwrite authored scene data"
    driver.click(232, 21)
    stopped = save_and_wait(lambda scene: scene["Entities"] == authored["Entities"])
    for key, value in authored.items():
        if key != "NextEntityID":
            assert stopped[key] == value, f"Stop did not restore authored {key}"
    assert stopped["NextEntityID"] >= authored["NextEntityID"], "Play reused persistent entity IDs"
    (artifacts / "Authored.aster").write_text(json.dumps(authored, indent=2) + "\n")
    (artifacts / "Stopped.aster").write_text(json.dumps(stopped, indent=2) + "\n")
    save_image(artifacts / "EditorAfter.ppm", driver.capture())
    report = {
        "logical_width": width, "logical_height": height, "gallery_id": gallery_id, "created_id": created_id,
        "play_changed_viewport_samples": changed, "viewport_sample_count": len(authored_samples),
        "gizmo_path": path, "moved_transform": transform, "restored_transform": original_transform,
        "assertions": ["native Save readiness", "world-X drag", "single Undo", "text rename", "Undo/Redo", "save/reload",
                       "entity creation", "play source preservation", "stop authored-state restoration"]
    }
    driver.close()
    # The platform owner publishes this report only after both the driver and
    # the editor have exited successfully. A close request alone is not proof.
    return report
