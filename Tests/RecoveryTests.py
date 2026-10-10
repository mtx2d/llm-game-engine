"""Recover real killed/EOF editor sessions without saving or clobbering sources."""

import argparse
import hashlib
import json
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading


class Editor:
    def __init__(self, executable, project, editor_state=None):
        arguments = [str(executable), "--automation", str(project)]
        if editor_state is not None:
            arguments.extend(["--editor-state", str(editor_state)])
        self.process = subprocess.Popen(arguments, text=True,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        while True:
            line = self.process.stdout.readline(1024 * 1024)
            self.lines.put(line)
            if not line:
                return

    def request(self, command, **fields):
        self.process.stdin.write(json.dumps(dict(command=command, **fields)) + "\n")
        self.process.stdin.flush()
        line = self.lines.get(timeout=20)
        assert line, "Editor exited before its recovery response"
        return json.loads(line)

    def get(self, command, **fields):
        response = self.request(command, **fields)
        assert response["ok"] and not response.get("warnings"), response
        return response["result"]

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=5)

    def eof(self):
        self.process.stdin.close()
        assert self.process.wait(timeout=10) == 0

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        try:
            if self.process.poll() is None:
                self.request("simulation.stop")
                self.get("session.close", discardChanges=True)
                assert self.process.wait(timeout=10) == 0
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=5)
            self.reader.join(timeout=5)
            for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
                stream.close()


def inactive(editor):
    return [entry for entry in editor.get("recovery.list") if not entry["active"]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("editor", "probe", "assets"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    args.editor, args.probe, args.assets = (getattr(args, name).resolve() for name in ("editor", "probe", "assets"))
    with tempfile.TemporaryDirectory(prefix="AsterRecovery-") as temporary:
        root = Path(temporary)
        unit = root / "Unit"
        unit.mkdir()
        subprocess.run([str(args.probe), "unit", str(unit)], check=True, timeout=30)
        # Independently check binary data, padding boundaries, multiple blocks
        # and a long message through the native implementation on every platform.
        payload = root / "Hash.bin"
        for count in (0, 1, 2, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 1000000):
            contents = bytes((index * 131 + 17) % 256 for index in range(count))
            payload.write_bytes(contents)
            result = subprocess.run([str(args.probe), "hash", str(payload)], check=True,
                                    capture_output=True, text=True, timeout=20)
            assert result.stdout.strip() == hashlib.sha256(contents).hexdigest(), count

        project = root / "Project"
        subprocess.run([str(args.editor), "--create-project", str(project), "Crash recovery"],
                       check=True, capture_output=True, text=True, timeout=15)
        source = project / "Assets/Scenes/Main.aster"
        original = source.read_bytes()
        with Editor(args.editor, project / "Project.asterproj") as owner:
            owner.get("entity.create", name="Crash survivor")
            authored = owner.get("scene.get")
            assert source.read_bytes() == original, "Automatic checkpoint silently saved source scene"
            with Editor(args.editor, project / "Project.asterproj") as other:
                entries = other.get("recovery.list")
                assert len(entries) == 1 and entries[0]["active"]
                session = entries[0]["id"]
                assert not other.request("recovery.restore", session=session)["ok"], "Recovered a live editor"
                assert not other.request("recovery.discard", session=session)["ok"], "Discarded a live editor"
                owner.kill()
                assert not other.get("recovery.list")[0]["active"]
                restored = other.get("recovery.restore", session=session)
                assert restored["path"] == "Scenes/Main.aster" and restored["warning"] is None
                assert other.get("scene.get") == authored and other.get("scene.status")["dirty"]
                assert source.read_bytes() == original, "Restore overwrote the authored source"
                other.get("recovery.discard", session=session)
                other.get("scene.save", path="Scenes/Main.aster")
                assert not other.get("scene.status")["dirty"] and other.get("recovery.list") == []
                assert json.loads(source.read_text()) == authored

        assets = root / "Assets"
        shutil.copytree(args.assets, assets)
        with Editor(args.editor, assets) as owner:
            owner.get("scene.load", path="Scenes/FeatureGallery.aster")
            owner.get("entity.create", name="Authored before play")
            authored = owner.get("scene.get")
            owner.get("simulation.start", audio="offline")
            owner.get("simulation.step", steps=30)
            assert owner.get("scene.get") != authored, "Play fixture did not mutate the runtime scene"
            owner.get("recovery.checkpoint")
            session = owner.get("recovery.list")[0]["id"]
            owner.kill()
        with Editor(args.editor, assets) as restored:
            restored.get("recovery.restore", session=session)
            assert restored.get("scene.get") == authored, "Recovery checkpointed transient play-state mutations"
            assert restored.get("scene.status")["dirty"] and not restored.get("scene.status")["playing"]
            restored.get("recovery.discard", session=session)
            restored.get("session.close", discardChanges=True)
            assert restored.process.wait(timeout=10) == 0
        with Editor(args.editor, assets) as empty:
            assert inactive(empty) == [], "Explicit discard/close left stale work"

        # A missing source recovers as an untitled copy, using asset-directory
        # mode even if the deleted scene was a project startup scene.
        missing = project / "Assets/Scenes/Main.aster"
        with Editor(args.editor, project / "Project.asterproj") as owner:
            owner.get("entity.create", name="Missing source survivor")
            authored = owner.get("scene.get")
            session = owner.get("recovery.list")[0]["id"]
            owner.eof()
        missing.unlink()
        with Editor(args.editor, project / "Assets") as restored:
            result = restored.get("recovery.restore", session=session)
            assert result["path"] is None and "unsaved copy" in result["warning"]
            assert restored.get("scene.get") == authored and not missing.exists()
            restored.get("scene.save", path="Scenes/Reconstructed.aster")
            restored.get("recovery.discard", session=session)

        broken = root / "Blocked Storage"
        broken.mkdir()
        (broken / ".aster").write_text("user-owned file")
        with Editor(args.editor, broken) as editor:
            response = editor.request("entity.create", name="Preserved after checkpoint error")
            assert response["ok"] and response["warnings"], "Automatic checkpoint failure was hidden"
            authored = editor.request("scene.get")["result"]
            assert authored["Entities"][0]["Name"] == "Preserved after checkpoint error"
            assert not editor.request("recovery.checkpoint")["ok"]
            assert (broken / ".aster").read_text() == "user-owned file"
            (broken / ".aster").rename(broken / "Preserved User File")
            editor.get("recovery.checkpoint")
            assert editor.get("scene.get") == authored, "Checkpoint retry changed pending work"
    print("Recovery: native unit/hash vectors, killed/EOF sessions, live ownership, play snapshots, missing sources and visible failures passed")


if __name__ == "__main__":
    main()
