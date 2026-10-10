"""Native lock contention/crash release and simultaneous editor publications."""

import json
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


def ready(process, expected):
    # Windows pipes cannot be selected. A single bounded reader owns the initial
    # line; communicate owns all remaining output after that reader has finished.
    result = queue.Queue()
    reader = threading.Thread(target=lambda: result.put(process.stdout.readline(4096)), daemon=True)
    reader.start()
    assert result.get(timeout=15).strip() == expected, "Native worker readiness failed"
    reader.join(timeout=1)
    assert not reader.is_alive()


def finish(process, request=""):
    output, errors = process.communicate(request, timeout=15)
    assert process.returncode == 0, errors
    return output.strip()


def main():
    executable = Path(sys.argv[1]).resolve()
    editor = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="AsterLocks-") as temporary:
        root = Path(temporary)
        unit = root / "Unit"
        unit.mkdir()
        subprocess.run([str(executable), "unit", str(unit)], check=True, timeout=15)
        lock_path = root / "Owner.lock"
        lock_path.write_text("persistent lock identity")
        original = lock_path.stat()
        for crash in (False, True):
            with subprocess.Popen([str(executable), "hold", str(lock_path)], text=True,
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as owner:
                try:
                    ready(owner, "locked")
                    contender = subprocess.run([str(executable), "probe", str(lock_path)],
                                               capture_output=True, text=True, timeout=10, check=True)
                    assert contender.stdout.strip() == "busy", "Another process bypassed ownership"
                    if crash:
                        owner.kill()
                        owner.wait(timeout=5)
                    else:
                        assert finish(owner, "release\n") == "released"
                finally:
                    if owner.poll() is None:
                        owner.kill()
                        owner.wait(timeout=5)
            # Windows may defer kernel lock cleanup briefly after termination.
            # Prove bounded eventual release without removing the stable file.
            deadline = time.monotonic() + 5
            while True:
                acquired = subprocess.run([str(executable), "probe", str(lock_path)],
                                          capture_output=True, text=True, timeout=10, check=True)
                if acquired.stdout.strip() == "locked":
                    break
                assert acquired.stdout.strip() == "busy" and time.monotonic() < deadline, \
                    "Process exit left stale ownership"
                time.sleep(0.02)
            assert lock_path.read_text() == "persistent lock identity"
            assert lock_path.stat().st_ino == original.st_ino, "Lock release replaced the stable file"

        project = root / "Project"
        subprocess.run([str(editor), "--create-project", str(project), "Save Race"],
                       check=True, capture_output=True, text=True, timeout=15)
        scene = project / "Assets/Scenes/Main.aster"
        # Both real editor document owners observe exactly the same baseline
        # before either can publish. Each trial permits only one winner.
        for mode, target in (("save", scene), ("configure", project / "Project.asterproj")):
            for trial in range(12):
                workers = []
                try:
                    for name in ("First", "Second"):
                        worker = subprocess.Popen([str(executable), mode, str(target), f"{name}-{trial}"],
                                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                                  stderr=subprocess.PIPE, text=True)
                        workers.append(worker)
                        ready(worker, "ready")
                    for worker in workers:
                        worker.stdin.write("save\n")
                        worker.stdin.flush()
                    results = [finish(worker) for worker in workers]
                    assert results.count("saved") == 1, f"Concurrent saves lost update protection: {results}"
                    rejected = next(result for result in results if result != "saved")
                    assert "Save conflict" in rejected or "being saved by another editor" in rejected, results
                    winner = "First" if results[0] == "saved" else "Second"
                    assert json.loads(target.read_text())["Name"] == f"{winner}-{trial}"
                finally:
                    for worker in workers:
                        if worker.poll() is None:
                            worker.kill()
                            worker.wait(timeout=5)
                        for stream in (worker.stdin, worker.stdout, worker.stderr):
                            stream.close()
        assert (scene.parent / ".aster/Writes.lock").is_file(), "Stable save lock was removed"
        assert (project / ".aster/ProjectWrites.lock").is_file(), "Stable configuration lock was removed"
    print("Native ownership, crash release and competing scene/configuration saves passed")


if __name__ == "__main__":
    main()
