"""Exercise the native editor through real X11 input; run inside xvfb-run."""

import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from NativeWindow import WindowRepaints


def project_editor_point(point, width=1440, height=900):
    """Project through the initial editor camera into its visible content area."""
    yaw, pitch = -0.61, -0.35
    forward = (math.cos(pitch) * math.sin(yaw), math.sin(pitch),
               -math.cos(pitch) * math.cos(yaw))
    right = (math.cos(yaw), 0, math.sin(yaw))
    up = (-math.sin(yaw) * math.sin(pitch), math.cos(pitch),
          math.cos(yaw) * math.sin(pitch))
    offset = tuple(value - origin for value, origin in zip(point, (7, 5, 10)))
    dot = lambda first, second: sum(a * b for a, b in zip(first, second))
    depth = dot(forward, offset)
    assert depth > 0, "Gizmo fixture must be in front of the editor camera"
    viewport_width, viewport_height = width - 590, height - 290
    focal_pixels = viewport_height / (2 * math.tan(math.radians(30)))
    return (260 + viewport_width / 2 + dot(right, offset) * focal_pixels / depth,
            74 + viewport_height / 2 - dot(up, offset) * focal_pixels / depth)


def close_window(window):
    # xdotool windowclose destroys the X window under bare Xvfb. Deliver the
    # window-manager close protocol so GLFW can retire its swapchain normally.
    class MessageData(ctypes.Union):
        _fields_ = [("bytes", ctypes.c_char * 20), ("shorts", ctypes.c_short * 10),
                    ("longs", ctypes.c_long * 5)]

    class ClientMessage(ctypes.Structure):
        _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int),
                    ("display", ctypes.c_void_p), ("window", ctypes.c_ulong),
                    ("message_type", ctypes.c_ulong), ("format", ctypes.c_int), ("data", MessageData)]

    class Event(ctypes.Union):
        _fields_ = [("client", ClientMessage), ("padding", ctypes.c_long * 24)]

    x11 = ctypes.CDLL("libX11.so.6")
    x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x11.XOpenDisplay.restype = ctypes.c_void_p
    x11.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    x11.XInternAtom.restype = ctypes.c_ulong
    x11.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long,
                              ctypes.POINTER(Event)]
    x11.XFlush.argtypes = [ctypes.c_void_p]
    x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
    display = x11.XOpenDisplay(None)
    assert display, "Cannot connect to test display"
    try:
        event = Event()
        event.client.type = 33  # ClientMessage
        event.client.send_event = 1
        event.client.display = display
        event.client.window = int(window)
        event.client.message_type = x11.XInternAtom(display, b"WM_PROTOCOLS", 0)
        event.client.format = 32
        event.client.data.longs[0] = x11.XInternAtom(display, b"WM_DELETE_WINDOW", 0)
        assert x11.XSendEvent(display, int(window), 0, 0, ctypes.byref(event))
        x11.XFlush(display)
    finally:
        x11.XCloseDisplay(display)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    args = parser.parse_args()
    if not os.environ.get("DISPLAY") or not shutil.which("xdotool"):
        raise RuntimeError("GUI integration requires DISPLAY and xdotool (use xvfb-run)")
    args.artifacts.mkdir(parents=True, exist_ok=True)
    screenshot = args.artifacts.resolve() / "EditorInteraction.ppm"
    with tempfile.TemporaryDirectory(prefix="AsterGui-") as temporary:
        project = Path(temporary) / "Assets"
        shutil.copytree(args.assets, project)
        scene_path = project / "Scenes/FeatureGallery.aster"
        baseline = json.loads(scene_path.read_text())
        # Picking must apply entity parents and glTF node transforms, ignore
        # invisible foreground geometry, and choose the cube before the floor.
        picked_cube = next(entity for entity in baseline["Entities"] if entity["Name"] == "DisplayCube")
        parent_id = baseline["NextEntityID"]
        parent_position = (-0.75, 0.2, -1)
        picked_cube["Parent"] = parent_id
        picked_cube["Transform"]["Scale"] = [1.6, 1.1, 0.8]
        picked_cube["MeshRenderer"]["Mesh"] = "Models/PickingCube.gltf"
        mesh = json.loads((project / "Models/Cube.gltf").read_text())
        mesh["nodes"][0]["translation"] = [0.75, 0, 0]
        (project / "Models/PickingCube.gltf").write_text(json.dumps(mesh))
        cube_center = tuple(parent_position[axis] + picked_cube["Transform"]["Translation"][axis] +
                            picked_cube["Transform"]["Scale"][axis] * mesh["nodes"][0]["translation"][axis]
                            for axis in range(3))
        baseline["Entities"].extend([
            {"ID": parent_id, "Name": "Picking parent", "Parent": None,
             "Transform": {"Translation": parent_position, "Rotation": [0, 0, 0], "Scale": [1, 1, 1]}},
            {"ID": parent_id + 1, "Name": "Invisible picking blocker", "Parent": None,
             "Transform": {"Translation": [(value + camera) / 2 for value, camera in zip(cube_center, (7, 5, 10))],
                           "Rotation": [0, 0, 0], "Scale": [1, 1, 1]},
             "MeshRenderer": dict(picked_cube["MeshRenderer"], Mesh="Models/Cube.gltf", Visible=False)}
        ])
        baseline["NextEntityID"] = parent_id + 2
        scene_path.write_text(json.dumps(baseline))
        (args.artifacts / "LastSaved.aster").unlink(missing_ok=True)
        with (args.artifacts / "EditorInteraction.log").open("w+") as log:
            process = subprocess.Popen(
                [str(args.editor.resolve()), "--project", str(project), "--audio", "offline",
                 "--screenshot", str(screenshot)], stdout=log, stderr=log)
            frames = None
            window = None
            try:
                def xdo(*arguments, check=True):
                    return subprocess.run(["xdotool", *map(str, arguments)], check=check,
                                          capture_output=True, text=True, timeout=5)

                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("Editor exited before showing its window")
                    result = xdo("search", "--onlyvisible", "--pid", process.pid, check=False)
                    if result.returncode == 0 and result.stdout.strip():
                        window = result.stdout.splitlines()[0]
                        break
                    time.sleep(0.05)
                assert window, "Editor did not show its native window"
                xdo("windowfocus", window)
                geometry = dict(line.split("=", 1) for line in
                                xdo("getwindowgeometry", "--shell", window).stdout.splitlines())
                frames = WindowRepaints(window, int(geometry["WIDTH"]), int(geometry["HEIGHT"]))
                frames.wait(process, timeout=30)

                def input_event(*arguments):
                    # One frame can already be rendering when an event arrives.
                    # Three complete redraw tails span a fresh GLFW poll and UI frame.
                    frames.drain()
                    xdo(*arguments)
                    frames.wait(process)

                def click(x, y):
                    input_event("mousemove", "--window", window, x, y)
                    input_event("mousedown", 1)
                    input_event("mouseup", 1)

                def save_and_wait(predicate):
                    deadline = time.monotonic() + 10
                    previous_write = scene_path.stat().st_mtime_ns
                    while time.monotonic() < deadline:
                        assert process.poll() is None, "Editor exited during interaction"
                        click(702, 21)
                        try:
                            saved = json.loads(scene_path.read_text())
                            if scene_path.stat().st_mtime_ns != previous_write and predicate(saved):
                                return saved
                        except (OSError, json.JSONDecodeError):
                            pass
                        time.sleep(0.1)
                    (args.artifacts / "LastSaved.aster").write_text(scene_path.read_text())
                    raise AssertionError("Native editor interaction did not persist the expected scene; see LastSaved.aster")

                # Actual controls must reach the shared authoring backend and serialization.
                gallery = next(entity for entity in baseline["Entities"] if entity["Name"] == "Gallery")
                original_transform = gallery["Transform"]

                def gallery_transform(scene):
                    return next(entity["Transform"] for entity in scene["Entities"] if entity["ID"] == gallery["ID"])

                click(55, 151)  # Select Gallery in the hierarchy before testing its world-X move handle.
                position = original_transform["Translation"]
                center = project_editor_point(position)
                axis_point = project_editor_point((position[0] + 1, position[1], position[2]))
                axis_delta = tuple(end - start for start, end in zip(center, axis_point))
                axis_length = math.hypot(*axis_delta)
                axis_direction = tuple(value / axis_length for value in axis_delta)
                start = tuple(value + direction * 45 for value, direction in zip(center, axis_direction))

                def drag_path(points, cancel_at=None):
                    input_event("mousemove", "--window", window, *map(round, points[0]))
                    input_event("mousedown", 1)
                    # Separate OS events across many rendered frames: one Undo must
                    # restore the entire drag, not merely its final movement increment.
                    for step, pointer in enumerate(points[1:], 1):
                        input_event("mousemove", "--window", window, *map(round, pointer))
                        if step == cancel_at:
                            input_event("key", "Escape")
                    input_event("mouseup", 1)

                axis_path = [tuple(value + direction * 55 * step / 8
                                   for value, direction in zip(start, axis_direction)) for step in range(9)]
                click(300, 21)  # Export modal leaves the gizmo visible in its background.
                drag_path(axis_path)
                input_event("key", "Escape")
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)

                drag_path(axis_path)  # The same path must work immediately after closing the modal.
                moved = save_and_wait(lambda scene: gallery_transform(scene)["Translation"][0] > position[0] + 0.25)
                moved_position = gallery_transform(moved)["Translation"]
                assert all(abs(moved_position[axis] - position[axis]) < 0.00001 for axis in (1, 2)), \
                    "Dragging the world-X handle changed another translation axis"
                click(147, 21)  # Exactly one Undo must coalesce the whole eight-event drag.
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)

                click(353, 106)  # Rotate around the front-facing world-Z ring, away from axis intersections.
                camera_right = (math.cos(-0.61), 0, math.sin(-0.61))
                right_point = project_editor_point(tuple(value + axis for value, axis in zip(position, camera_right)))
                right_pixels = math.dist(center, right_point)
                # ImGuizmo's default handle length is 0.1 of the clip-space width;
                # its rotation rings have 1.2 times that world-space radius.
                rotation_radius = 0.1 * 1440 / (2 * right_pixels) * 1.2
                rotation_path = [project_editor_point((position[0] + rotation_radius * math.cos(angle),
                                                       position[1] + rotation_radius * math.sin(angle), position[2]))
                                 for angle in (0.35 + 0.5 * step / 8 for step in range(9))]
                drag_path(rotation_path)
                rotated = save_and_wait(lambda scene: abs(gallery_transform(scene)["Rotation"][2]) > 0.2)
                rotated_transform = gallery_transform(rotated)
                assert all(abs(angle) < 0.00001 for angle in rotated_transform["Rotation"][:2]), \
                    "World-Z rotation handle changed another rotation axis"
                assert rotated_transform["Translation"] == original_transform["Translation"], \
                    "Rotation gizmo changed entity position"
                assert all(abs(value - 1) < 0.00001 for value in rotated_transform["Scale"]), \
                    "Rotation gizmo changed entity scale"
                click(147, 21)
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)

                click(426, 106)  # Scale only local X (the restored fixture has identity rotation).
                drag_path(axis_path)
                scaled = save_and_wait(lambda scene: gallery_transform(scene)["Scale"][0] > 1.25)
                scaled_transform = gallery_transform(scaled)
                assert all(abs(value - 1) < 0.00001 for value in scaled_transform["Scale"][1:]), \
                    "X scale handle changed another scale axis"
                assert scaled_transform["Translation"] == original_transform["Translation"] and \
                    scaled_transform["Rotation"] == original_transform["Rotation"], \
                    "Scale gizmo changed entity position or rotation"
                click(147, 21)
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)
                click(294, 106)
                drag_path(axis_path, cancel_at=4)
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)

                def rename_selected(name):
                    click(1190, 221)
                    # Keep modifiers held across acknowledged UI frames. Packing
                    # the chord can release Control before the queued A is consumed.
                    input_event("keydown", "Control_L")
                    input_event("keydown", "a")
                    input_event("keyup", "a")
                    input_event("keyup", "Control_L")
                    input_event("type", "--clearmodifiers", "--delay", 20, name)
                    input_event("key", "Return")

                cube_pointer = tuple(map(round, project_editor_point(cube_center)))
                click(300, 21)
                click(*cube_pointer)  # A modal must also prevent changing viewport selection.
                input_event("key", "Escape")
                rename_selected("ModalSelectionProbe")
                save_and_wait(lambda scene: any(entity["ID"] == gallery["ID"] and
                                               entity["Name"] == "ModalSelectionProbe" for entity in scene["Entities"]))
                click(147, 21)
                save_and_wait(lambda scene: any(entity["ID"] == gallery["ID"] and entity["Name"] == "Gallery"
                                               for entity in scene["Entities"]))
                click(*cube_pointer)
                rename_selected("ViewportPickedCube")
                save_and_wait(lambda scene: any(entity["ID"] == picked_cube["ID"] and
                                               entity["Name"] == "ViewportPickedCube" for entity in scene["Entities"]))
                click(147, 21)
                save_and_wait(lambda scene: any(entity["ID"] == picked_cube["ID"] and entity["Name"] == "DisplayCube"
                                               for entity in scene["Entities"]))
                click(1060, 150)  # Empty sky clears selection; Delete must then be disabled.
                click(178, 114)
                save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]))

                click(55, 114)
                created = save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]) + 1)
                created_id = max(entity["ID"] for entity in created["Entities"])
                rename_selected("GuiTestEntity")
                save_and_wait(lambda scene: any(entity["ID"] == created_id and entity["Name"] == "GuiTestEntity"
                                               for entity in scene["Entities"]))
                click(147, 21)  # Undo rename.
                save_and_wait(lambda scene: any(entity["ID"] == created_id and entity["Name"] == "Entity"
                                               for entity in scene["Entities"]))
                click(147, 21)  # Undo creation.
                save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]))
                click(190, 21)  # Redo creation.
                authored = save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]) + 1)
                click(232, 21)  # Play: Lua, Bullet and native input run through the GUI adapter.
                time.sleep(0.5)
                play_write = scene_path.stat().st_mtime_ns
                click(702, 21)
                assert scene_path.stat().st_mtime_ns == play_write, "Play did not disable authoring Save"
                click(232, 21)  # Stop restores authored state.
                stopped = save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]) + 1)
                original_sphere = next(entity for entity in authored["Entities"] if entity["Name"] == "FallingSphere")
                stopped_sphere = next(entity for entity in stopped["Entities"] if entity["Name"] == "FallingSphere")
                assert stopped_sphere == original_sphere, "GUI stop did not restore authored simulation state"
                close_window(window)
                assert process.wait(timeout=15) == 0, "Editor reported a native or Vulkan validation error"
            finally:
                if frames:
                    frames.close()
                if process.poll() is None:
                    try:
                        if window:
                            close_window(window)
                        else:
                            process.terminate()
                        process.wait(timeout=15)
                    except (OSError, subprocess.TimeoutExpired) as error:
                        print(f"Native editor cleanup required termination: {error}")
                        process.terminate()
                        process.wait(timeout=5)
                log.seek(0)
                diagnostics = log.read()
                if diagnostics:
                    print(diagnostics)
        with screenshot.open("rb") as image:
            assert image.readline() == b"P6\n"
            assert image.readline() == b"1440 900\n"
            assert image.readline() == b"255\n"
            pixels = image.read()
        assert len(pixels) == 1440 * 900 * 3
        colors = set(zip(pixels[0::3], pixels[1::3], pixels[2::3]))
        assert len(colors) > 500, "Editor screenshot lacks the rendered scene and GUI"
        print("Native editor transformed-mesh picking, modal isolation, move/rotate/scale with undo/cancel, "
              "create, rename, undo/redo, save, play/stop and GPU readback passed")


if __name__ == "__main__":
    main()
