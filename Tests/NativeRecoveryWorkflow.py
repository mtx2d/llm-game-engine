"""One actual GUI crash/recovery workflow for native platform input drivers."""

from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

from NativeEditorWorkflow import save_image


@contextmanager
def lock_catalog(path):
    with path.open("r+b") as lock:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        locked = True

        def release():
            nonlocal locked
            if locked:
                if os.name == "nt":
                    lock.seek(0)
                    msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(lock, fcntl.LOCK_UN)
                locked = False

        try:
            yield release
        finally:
            release()


def run_recovery_workflow(executable, source_assets, artifacts, create_driver):
    artifacts = Path(artifacts)
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "RecoveryGui.json").unlink(missing_ok=True)

    @contextmanager
    def session(assets, stage):
        evidence = artifacts / stage
        evidence.mkdir(exist_ok=True)
        with (evidence / "Editor.log").open("w+") as log, (evidence / "Input.stderr").open("w+") as diagnostics:
            process = subprocess.Popen([str(executable), "--project", str(assets), "--audio", "offline"],
                                        stdout=log, stderr=log)
            driver = None
            try:
                driver = create_driver(process, evidence, diagnostics)
                driver.initialize()
                yield process, driver, evidence
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
                           for marker in ("Validation Error", "VUID-", "NVRHI Error", "Aster editor:")), \
                "Native recovery editor reported an error; inspect Editor.log"

    def wait(driver, predicate, message, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            driver.assert_alive()
            result = predicate()
            if result:
                return result
            time.sleep(0.05)
        raise AssertionError(message)

    def modal(driver, evidence, stage, visible=True):
        width, height = driver.size()

        def matches():
            image = driver.capture()
            pixel_width, pixel_height, pixels = image
            dark = 0
            # The dialog body is dark; this region excludes labels and buttons.
            for row in range(20):
                y = int((height / 2 - 40 + row * 114 / 19) * pixel_height / height)
                for column in range(52):
                    x = int((width / 2 - 260 + column * 510 / 51) * pixel_width / width)
                    offset = (y * pixel_width + x) * 3
                    dark += max(pixels[offset:offset + 3]) < 80
            return (image, dark) if (dark > 936) == visible else None

        image, dark = wait(driver, matches, f"Recovery modal did not become visible={visible}")
        save_image(evidence / f"{stage}.ppm", image)
        (evidence / f"{stage}.json").write_text(json.dumps(
            {"dark_samples": dark, "samples": 1040, "visible": visible}))
        print(f"Recovery {stage}: {dark}/1040 dark samples, visible={visible}", flush=True)

    with tempfile.TemporaryDirectory(prefix="AsterNativeRecovery-") as temporary:
        assets = Path(temporary) / "Assets"
        shutil.copytree(source_assets, assets)
        source = assets / "Scenes/FeatureGallery.aster"
        recovery = assets / ".aster/Recovery"

        def manifests():
            return sorted(recovery.glob("*/Manifest.json"))

        with session(assets, "BeforeCrash") as (process, driver, evidence):
            width, height = driver.size()
            before_save = source.stat().st_mtime_ns

            def ready():
                driver.click(702, 21)
                return source.stat().st_mtime_ns != before_save

            wait(driver, ready, "Native GUI Save readiness failed")
            original = source.read_bytes()
            driver.click(55, 151)
            driver.replace_text(width - 250, 221, "Recovered GUI work")

            def checkpointed():
                entries = manifests()
                if len(entries) != 1:
                    return None
                manifest = json.loads(entries[0].read_text())
                contents = (entries[0].parent / manifest["Checkpoint"]).read_bytes()
                assert hashlib.sha256(contents).hexdigest() == manifest["CheckpointDigest"]
                scene = json.loads(contents)
                return scene if scene["Entities"][0]["Name"] == "Recovered GUI work" else None

            authored = wait(driver, checkpointed, "Native GUI change was not automatically checkpointed")
            assert source.read_bytes() == original, "Automatic checkpoint silently saved the scene"
            dead_session = manifests()[0].parent
            save_image(evidence / "Authored.ppm", driver.capture())
            if hasattr(driver, "detach"):
                driver.detach()  # Retire the native input helper before intentionally killing its target.
            process.kill()
            process.wait(timeout=10)

        with lock_catalog(recovery / "Catalog.lock") as release_catalog, \
             session(assets, "AfterCrash") as (process, driver, evidence):
            width, height = driver.size()
            modal(driver, evidence, "BusyCatalog", visible=False)
            release_catalog()
            # Startup discovery must retry automatically after temporary contention.
            modal(driver, evidence, "Startup")
            assert source.read_bytes() == original, "Startup recovery silently modified the source"
            driver.click(width / 2 + 180, height / 2 + 106)  # Later retains work.
            modal(driver, evidence, "Later", visible=False)
            assert (dead_session / "Manifest.json").exists()
            driver.click(755, 21)
            modal(driver, evidence, "Reopened")
            driver.click(width / 2 - 230, height / 2 - 80)
            driver.click(width / 2 - 200, height / 2 + 106)
            modal(driver, evidence, "Restored", visible=False)
            wait(driver, lambda: len(manifests()) == 2, "Adopted work was not checkpointed before publication")
            assert source.read_bytes() == original, "Recover copy silently saved the scene"
            before_save = source.stat().st_mtime_ns
            driver.click(702, 21)
            wait(driver, lambda: source.stat().st_mtime_ns != before_save and json.loads(source.read_text()) == authored,
                 "Recovered scene did not save exactly to its original filename")
            wait(driver, lambda: len(manifests()) == 1, "Save retained the editor's owned checkpoint")
            driver.click(755, 21)
            modal(driver, evidence, "BeforeDiscard")
            driver.click(width / 2 - 230, height / 2 - 80)
            driver.click(width / 2 - 20, height / 2 + 106)
            wait(driver, lambda: not dead_session.exists(), "Explicit native discard did not remove the dead session")
            assert not manifests() and json.loads(source.read_text()) == authored
            driver.click(width / 2 + 180, height / 2 + 106)
            modal(driver, evidence, "Discarded", visible=False)
            driver.close()
            assert process.wait(timeout=20) == 0, "Saved recovered editor did not close cleanly"
        (artifacts / "Recovered.aster").write_text(json.dumps(authored, indent=2))
        (artifacts / "RecoveryGui.json").write_text(json.dumps({"passed": True,
            "source_preserved_until_save": True, "saved_scene_exact": True, "explicit_discard": True}))
        print("Native GUI crash/recovery, Later, exact Save and explicit discard passed", flush=True)
