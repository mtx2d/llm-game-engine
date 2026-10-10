"""Project controls exercised only through real native GUI input after launch."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from NativeEditorWorkflow import save_image
from RecoveryTests import Editor as AutomationEditor


def run_startup_repair_workflow(executable, artifacts, create_driver):
    """An ordinary interactive launch must expose repair for corrupt/missing startup."""
    results = []
    for missing in (False, True):
        evidence = artifacts / ("MissingStartup" if missing else "CorruptStartup")
        evidence.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="AsterRepair-") as temporary:
            root = Path(temporary).resolve() / "Project"
            subprocess.run([str(executable), "--create-project", str(root), "Repair"],
                           check=True, capture_output=True, timeout=30)
            project = root / "Project.asterproj"
            startup = root / "Assets/Scenes/Main.aster"
            replacement = root / "Assets/Scenes/Ready.aster"
            shutil.copyfile(startup, replacement)
            expected_scene = json.loads(replacement.read_text())
            if missing:
                startup.unlink()
            else:
                startup.write_bytes(b"Damaged startup kept for manual inspection")
            original_config = project.read_bytes()
            # Bounded/verification launches retain strict failure behavior.
            strict = subprocess.run([str(executable), "--project", str(project), "--frames", "1"],
                                    capture_output=True, timeout=30)
            assert strict.returncode != 0 and b"Aster editor:" in strict.stderr
            with (evidence / "Editor.log").open("w+") as log, (evidence / "Input.stderr").open("w+") as diagnostics:
                process = subprocess.Popen([str(executable), "--project", str(project), "--audio", "offline"],
                                            stdout=log, stderr=log)
                driver = None
                try:
                    driver = create_driver(process, evidence, diagnostics)
                    driver.initialize()
                    width, height = driver.size()
                    left, top = width / 2 - 320, height / 2 - 210

                    def wait(predicate, message):
                        deadline = time.monotonic() + 30
                        while time.monotonic() < deadline:
                            driver.assert_alive()
                            result = predicate()
                            if result:
                                return result
                            time.sleep(0.05)
                        save_image(evidence / "Failure.ppm", driver.capture())
                        raise AssertionError(message)

                    def repair_presented():
                        image = driver.capture()
                        pw, ph, pixels = image
                        blue = 0
                        for x in range(20, 600, 3):
                            offset = (int((top + 10) * ph / height) * pw +
                                      int((left + x) * pw / width)) * 3
                            r, g, b = pixels[offset:offset + 3]
                            blue += b > 90 and g > r + 15
                        return image if blue > 150 else None

                    save_image(evidence / "StartupRepair.ppm", wait(
                        repair_presented, "Interactive launch did not present its startup repair controls"))
                    assert project.read_bytes() == original_config
                    if missing:
                        assert not startup.exists()
                    else:
                        assert startup.read_bytes() == b"Damaged startup kept for manual inspection"
                    driver.replace_text(left + 150, top + 277, "Scenes/Ready.aster")
                    driver.click(left + 90, top + 316)
                    wait(lambda: json.loads(project.read_text())["StartScene"] == "Scenes/Ready.aster",
                         "GUI did not persist the repaired startup configuration")
                    before_save = replacement.stat().st_mtime_ns
                    driver.click(702, 21)
                    wait(lambda: replacement.stat().st_mtime_ns != before_save and
                         json.loads(replacement.read_text()) == expected_scene,
                         "Repaired startup did not become the active saved document")
                    if missing:
                        assert not startup.exists()
                    else:
                        assert startup.read_bytes() == b"Damaged startup kept for manual inspection"
                    save_image(evidence / "Repaired.ppm", driver.capture())
                    driver.close()
                    assert process.wait(timeout=30) == 0, "Startup repair editor failed normal close"
                finally:
                    try:
                        if driver:
                            try:
                                driver.cleanup()
                            finally:
                                (evidence / "Input.json").write_text(json.dumps(driver.events, indent=2))
                    finally:
                        if process.poll() is None:
                            process.kill()
                            process.wait(timeout=10)
                log.flush()
                assert not any(marker in (evidence / "Editor.log").read_text()
                               for marker in ("Validation Error", "VUID-", "NVRHI Error", "Aster editor:"))
            result = {"passed": True, "missing": missing, "source_preserved": True,
                      "configuration_repaired": True, "active_scene_exact": True}
            (evidence / "Repair.json").write_text(json.dumps(result, indent=2))
            results.append(result)
    return results


def run_project_workflow(executable, source_assets, artifacts, create_driver):
    artifacts = Path(artifacts)
    artifacts.mkdir(parents=True, exist_ok=True)
    for pattern in ("*.ppm", "*.png", "Projects.json", "Input.json"):
        for previous in artifacts.glob(pattern):
            previous.unlink()
    for name in ("CorruptStartup", "MissingStartup"):
        evidence = artifacts / name
        for pattern in ("*.ppm", "*.png", "Repair.json", "Input.json"):
            for previous in evidence.glob(pattern):
                previous.unlink()
    with tempfile.TemporaryDirectory(prefix="AsterProjects-") as temporary:
        root = Path(temporary).resolve()
        primary = root / "Primary"
        subprocess.run([str(executable), "--create-project", str(primary), "Primary"],
                       check=True, capture_output=True, timeout=30)
        project_file = primary / "Project.asterproj"
        assets = primary / "Assets"
        (assets / "Models").mkdir()
        mesh = json.loads((source_assets / "Models/Cube.gltf").read_text())
        mesh["extensionsUsed"] = ["KHR_materials_unlit"]
        mesh["materials"][0]["extensions"] = {"KHR_materials_unlit": {}}
        mesh["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = [0.9, 0.02, 0.01, 1]
        (assets / "Models/Cube.gltf").write_text(json.dumps(mesh))
        with AutomationEditor(executable, project_file) as preparation:
            entity = preparation.get("entity.create", name="Project mesh")["entity"]
            preparation.get("entity.patch", entity=entity, patch={
                "Transform": {"Translation": [-2, 1, 0], "Scale": [2, 2, 2]},
                "MeshRenderer": {"Mesh": "Models/Cube.gltf", "BaseColor": [1, 1, 1, 1],
                                 "Metallic": 0, "Roughness": 0.5, "Visible": True}})
            preparation.get("scene.save", path="Scenes/Main.aster")
        shutil.copyfile(assets / "Scenes/Main.aster", assets / "Scenes/Other.aster")
        alternate = primary / "AlternateAssets"
        shutil.copytree(assets, alternate, ignore=shutil.ignore_patterns(".aster"))
        mesh["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = [0.01, 0.02, 0.9, 1]
        (alternate / "Models/Cube.gltf").write_text(json.dumps(mesh))
        created = root / "Created"

        with (artifacts / "Editor.log").open("w+") as log, (artifacts / "Input.stderr").open("w+") as diagnostics:
            process = subprocess.Popen([str(executable), "--project", str(project_file), "--audio", "offline"],
                                        cwd=root, stdout=log, stderr=log)
            driver = None
            try:
                driver = create_driver(process, artifacts, diagnostics)
                driver.initialize()
                width, height = driver.size()
                left, top = width / 2 - 320, height / 2 - 210

                def wait(predicate, message, timeout=30):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        driver.assert_alive()
                        result = predicate()
                        if result:
                            return result
                        time.sleep(0.05)
                    save_image(artifacts / "Failure.ppm", driver.capture())
                    raise AssertionError(message)

                def colored(channel, stage):
                    def visible():
                        image = driver.capture()
                        pixel_width, pixel_height, pixels = image
                        count = 0
                        for y in range(140, height - 226, 2):
                            for x in range(270, width - 340, 2):
                                offset = (int(y * pixel_height / height) * pixel_width +
                                          int(x * pixel_width / width)) * 3
                                rgb = pixels[offset:offset + 3]
                                count += rgb[channel] > 100 and all(
                                    rgb[channel] > rgb[other] + 60 for other in range(3) if other != channel)
                        return (image, count) if count > 100 else None

                    image, count = wait(visible, f"{stage}: new asset root did not produce its expected mesh color")
                    save_image(artifacts / f"{stage}.ppm", image)
                    return count

                def projects():
                    driver.click(824, 21)
                    # Require the actual presented blue title bar. Dark viewport
                    # pixels alone do not establish that a dialog is visible.
                    def presented():
                        image = driver.capture()
                        pw, ph, pixels = image
                        samples = [pixels[(int((top + 10) * ph / height) * pw +
                                           int((left + x) * pw / width)) * 3:][:3]
                                   for x in range(20, 600, 3)]
                        return image if sum(b > 90 and g > r + 15 for r, g, b in samples) > 150 else None
                    image = wait(presented, "Projects window was not presented")
                    save_image(artifacts / "ProjectsWindow.ppm", image)

                def confirmation(stage, visible=True):
                    def matches():
                        image = driver.capture()
                        pw, ph, pixels = image
                        count = 0
                        for y in range(0, 18, 3):
                            for x in range(-220, 220, 3):
                                offset = (int((height / 2 + y) * ph / height) * pw +
                                          int((width / 2 + x) * pw / width)) * 3
                                r, g, b = pixels[offset:offset + 3]
                                count += r > 30 and b > r + 20 and g > r + 10
                        return image if (count > 300) == visible else None
                    image = wait(matches, f"Document confirmation visible={visible} was not presented")
                    save_image(artifacts / f"{stage}.ppm", image)

                def failure_presented(before, stage):
                    def changed():
                        image = driver.capture()
                        pw, ph, pixels = image
                        assert image[:2] == before[:2]
                        start = int((height - 23) * ph / height) * pw * 3
                        end = int((height - 7) * ph / height) * pw * 3
                        different = sum(abs(a - b) > 20 for a, b in zip(
                            before[2][start:end], pixels[start:end]))
                        return image if different > 64 else None
                    save_image(artifacts / f"{stage}.ppm", wait(changed, "Expected error status was not presented"))

                def field(x, y, text):
                    driver.replace_text(left + x, top + y, str(text))

                def apply_config():
                    driver.click(left + 90, top + 316)

                def save_scene(path, predicate=lambda scene: True):
                    previous = path.stat().st_mtime_ns
                    driver.click(702, 21)
                    return wait(lambda: path.stat().st_mtime_ns != previous and
                                predicate(json.loads(path.read_text())), "Scene save did not match the active project")

                red_pixels = colored(0, "InitialRed")
                original = assets / "Scenes/Main.aster"
                save_scene(original)
                source_bytes = original.read_bytes()
                driver.click(55, 151)  # Camera is the first hierarchy entity.
                driver.replace_text(width - 250, 171, "Pending project work")

                def checkpointed():
                    for manifest in (assets / ".aster/Recovery").glob("*/Manifest.json"):
                        metadata = json.loads(manifest.read_text())
                        scene = json.loads((manifest.parent / metadata["Checkpoint"]).read_text())
                        if scene["Entities"][0]["Name"] == "Pending project work":
                            return scene
                    return None

                authored = wait(checkpointed, "Pending project edit was not completed/checkpointed")
                projects()
                field(150, 221, "Configured in GUI")
                field(150, 277, "Scenes/Missing.aster")
                before_failure = driver.capture()
                unchanged_config = project_file.read_bytes()
                apply_config()
                failure_presented(before_failure, "RejectedStartup")
                assert project_file.read_bytes() == unchanged_config and checkpointed() == authored
                assert original.read_bytes() == source_bytes
                field(150, 277, "Scenes/Other.aster")
                apply_config()
                wait(lambda: json.loads(project_file.read_text())["Name"] == "Configured in GUI",
                     "GUI configuration did not persist")
                assert json.loads(project_file.read_text())["StartScene"] == "Scenes/Other.aster"
                assert original.read_bytes() == source_bytes and checkpointed() == authored
                colored(0, "SameRootPreserved")
                projects()
                field(150, 249, "AlternateAssets")
                apply_config()  # Dirty root change requires the shared confirmation.
                confirmation("DirtyRootConfirmation")
                driver.click(width / 2 + 144, height / 2 + 8)  # Cancel.
                confirmation("CancelledRootChange", visible=False)
                assert json.loads(project_file.read_text())["AssetDirectory"] == "Assets"
                assert checkpointed() == authored and original.read_bytes() == source_bytes
                apply_config()
                confirmation("ConfirmedRootChange")
                driver.click(width / 2, height / 2 + 8)  # Explicit Discard.
                wait(lambda: json.loads(project_file.read_text())["AssetDirectory"] == "AlternateAssets",
                     "Confirmed GUI asset-root change did not persist")
                blue_pixels = colored(2, "AlternateBlue")
                assert original.read_bytes() == source_bytes
                active = alternate / "Scenes/Other.aster"
                save_scene(active)
                assert json.loads(active.read_text())["Entities"][0]["Name"] == "Camera"
                driver.click(158, 21)  # Undo cannot restore an old document.
                save_scene(active)
                assert json.loads(active.read_text())["Entities"][0]["Name"] == "Camera"

                projects()
                field(150, 125, "Created")
                field(150, 153, "Created in GUI")
                driver.click(left + 560, top + 153)
                created_file = created / "Project.asterproj"
                wait(created_file.is_file, "GUI project creation did not publish")
                assert json.loads(created_file.read_text())["Name"] == "Created in GUI"
                created_scene = created / "Assets/Scenes/Main.aster"
                save_scene(created_scene)
                assert len(json.loads(created_scene.read_text())["Entities"]) == 1
                projects()
                field(150, 65, "Missing.asterproj")
                before_failure = driver.capture()
                created_bytes = created_scene.read_bytes()
                driver.click(left + 560, top + 65)
                failure_presented(before_failure, "RejectedOpen")
                assert created_scene.read_bytes() == created_bytes
                field(150, 65, "Primary/Project.asterproj")
                driver.click(left + 560, top + 65)
                colored(2, "ReopenedBlue")
                driver.click(55, 151)
                driver.replace_text(width - 250, 171, "Saved before project switch")
                projects()
                field(150, 65, "Created/Project.asterproj")
                driver.click(left + 560, top + 65)
                confirmation("SaveBeforeOpen")
                driver.click(width / 2 - 153, height / 2 + 8)  # Save and continue.
                wait(lambda: json.loads(active.read_text())["Entities"][0]["Name"] ==
                     "Saved before project switch", "Project open did not save the old authored document")
                save_scene(created_scene)
                assert len(json.loads(created_scene.read_text())["Entities"]) == 1
                assert original.read_bytes() == source_bytes
                driver.close()
                assert process.wait(timeout=30) == 0, "Project GUI shutdown failed"
                report = {"passed": True, "initial_red_samples": red_pixels,
                          "alternate_blue_samples": blue_pixels, "same_root_preserved": True,
                          "dirty_cancel": True, "invalid_startup_and_open_preserved": True,
                          "root_cache_rebound": True,
                          "created_and_reopened": True, "save_before_project_switch": True}
            finally:
                try:
                    if driver:
                        try:
                            driver.cleanup()
                        finally:
                            (artifacts / "Input.json").write_text(json.dumps(driver.events, indent=2))
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=10)
            log.flush()
            assert not any(marker in (artifacts / "Editor.log").read_text()
                           for marker in ("Validation Error", "VUID-", "NVRHI Error", "Aster editor:"))
        report["startup_repair"] = run_startup_repair_workflow(executable, artifacts, create_driver)
        (artifacts / "Projects.json").write_text(json.dumps(report, indent=2))
    print("Native GUI project configuration, dirty protection, root rebinding, creation and opening passed")
