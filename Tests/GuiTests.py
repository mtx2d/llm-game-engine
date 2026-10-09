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
        with (args.artifacts / "EditorInteraction.log").open("w+") as log:
            process = subprocess.Popen(
                [str(args.editor.resolve()), "--project", str(project), "--audio", "offline",
                 "--screenshot", str(screenshot)], stdout=log, stderr=log)
            try:
                def xdo(*arguments, check=True):
                    return subprocess.run(["xdotool", *map(str, arguments)], check=check,
                                          capture_output=True, text=True, timeout=5)

                deadline = time.monotonic() + 30
                window = None
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
                time.sleep(1)

                def click(x, y):
                    xdo("mousemove", "--window", window, x, y)
                    xdo("mousedown", 1)
                    time.sleep(0.08)
                    xdo("mouseup", 1)
                    time.sleep(0.12)

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
                    raise AssertionError("Native editor interaction did not persist the expected scene")

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

                def drag_handle():
                    xdo("mousemove", "--window", window, *map(round, start))
                    time.sleep(0.12)
                    xdo("mousedown", 1)
                    time.sleep(0.12)
                    # Separate OS events across many rendered frames: one Undo must
                    # restore the entire drag, not merely its final movement increment.
                    for step in range(1, 9):
                        pointer = tuple(value + direction * 55 * step / 8
                                        for value, direction in zip(start, axis_direction))
                        xdo("mousemove", "--window", window, *map(round, pointer))
                        time.sleep(0.08)
                    xdo("mouseup", 1)
                    time.sleep(0.12)

                click(300, 21)  # Export modal leaves the gizmo visible in its background.
                drag_handle()
                xdo("key", "Escape")
                time.sleep(0.2)
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)

                drag_handle()  # The same path must work immediately after closing the modal.
                moved = save_and_wait(lambda scene: gallery_transform(scene)["Translation"][0] > position[0] + 0.25)
                moved_position = gallery_transform(moved)["Translation"]
                assert all(abs(moved_position[axis] - position[axis]) < 0.00001 for axis in (1, 2)), \
                    "Dragging the world-X handle changed another translation axis"
                click(147, 21)  # Exactly one Undo must coalesce the whole eight-event drag.
                save_and_wait(lambda scene: gallery_transform(scene) == original_transform)

                click(55, 114)
                created = save_and_wait(lambda scene: len(scene["Entities"]) == len(baseline["Entities"]) + 1)
                created_id = max(entity["ID"] for entity in created["Entities"])
                click(1190, 221)
                xdo("key", "--clearmodifiers", "ctrl+a")
                xdo("type", "--clearmodifiers", "--delay", 20, "GuiTestEntity")
                xdo("key", "Return")
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
                if process.poll() is None:
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
        print("Native editor modal isolation, gizmo drag/single undo, create, rename, undo/redo, save, play/stop and GPU readback passed")


if __name__ == "__main__":
    main()
