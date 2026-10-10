"""Exercise the shipped command protocol and runtime error reporting as processes."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


editor, runtime = (str(Path(argument).resolve()) for argument in sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix="AsterProtocol-") as temporary:
    project = Path(temporary)
    requests = [
        "not-json",
        json.dumps({"id": 1, "command": "entity.create", "name": "Protocol"}),
        json.dumps({"id": 2, "command": "scene.save", "path": "Scene.aster"}),
        json.dumps({"id": 3, "command": "scene.get"}),
    ]
    process = subprocess.run(
        [editor, "--automation", str(project)],
        input="\n".join(requests) + "\n", text=True, capture_output=True, timeout=30,
    )
    assert process.returncode == 0, process.stderr
    responses = [json.loads(line) for line in process.stdout.splitlines()]
    assert len(responses) == 4, process.stdout
    assert not responses[0]["ok"]
    assert all(response["ok"] for response in responses[1:]), responses
    assert [response["id"] for response in responses[1:]] == [1, 2, 3]
    assert responses[3]["result"]["Entities"][0]["Name"] == "Protocol"
    process = subprocess.run(
        [runtime, "--scene", str(project / "Scene.aster"), "--project", str(project), "--steps", "3"],
        text=True, capture_output=True, timeout=30,
    )
    assert process.returncode == 0, process.stderr
    result = json.loads(process.stdout)
    assert result["errors"] == [] and result["steps"] == 3 and result["frames"] == 0
    invalid_validation = subprocess.run(
        [runtime, "--scene", str(project / "Scene.aster"), "--project", str(project),
         "--steps", "0", "--validation"], text=True, capture_output=True, timeout=30,
    )
    assert invalid_validation.returncode != 0 and "requires a graphical" in invalid_validation.stderr

    # Teardown is part of the runtime result, including an OnDestroy failure.
    (project / "Teardown.lua").write_text(
        'return {OnDestroy=function(self, entity) error("teardown sentinel") end}', encoding="utf-8"
    )
    scene = responses[3]["result"]
    scene["Entities"][0]["Script"] = {"Path": "Teardown.lua", "Enabled": True}
    (project / "Scene.aster").write_text(json.dumps(scene), encoding="utf-8")
    process = subprocess.run(
        [runtime, "--scene", str(project / "Scene.aster"), "--project", str(project), "--steps", "0"],
        text=True, capture_output=True, timeout=30,
    )
    assert process.returncode == 1, process.stdout + process.stderr
    assert any("teardown sentinel" in error for error in json.loads(process.stdout)["errors"])

    # EOF also ends an active editor play session. It must execute OnDestroy
    # before reporting success, without emitting an unsolicited JSON response.
    requests = [{"command": "scene.load", "path": "Scene.aster"},
                {"command": "simulation.start", "audio": "offline"}]
    for script, expected_code in (
        ('return {OnDestroy=function(self, entity) error("editor teardown sentinel") end}', 1),
        ('return {OnDestroy=function(self, entity) engine.set_name(entity, "Stopped") end}', 0),
    ):
        (project / "Teardown.lua").write_text(script, encoding="utf-8")
        process = subprocess.run(
            [editor, "--automation", str(project)],
            input="\n".join(json.dumps(request) for request in requests) + "\n",
            text=True, capture_output=True, timeout=30,
        )
        assert process.returncode == expected_code, process.stdout + process.stderr
        results = [json.loads(line) for line in process.stdout.splitlines()]
        assert len(results) == 2 and all(result["ok"] for result in results), results
        assert results[1]["result"]["errors"] == [], "Teardown ran before EOF"
        assert ("editor teardown sentinel" in process.stderr) == (expected_code == 1)
    for value in ("nan", "2.5", "-1", "1000001", "3junk"):
        process = subprocess.run([runtime, "--steps", value], text=True, capture_output=True, timeout=30)
        assert process.returncode != 0

    requests = [{"command": "entity.create"}, {"command": "session.close"},
                {"command": "scene.status"}, {"command": "session.close", "discardChanges": True},
                {"command": "entity.create", "name": "Must not run after close"}]
    process = subprocess.run([editor, "--automation", str(project)],
                             input="".join(json.dumps(request) + "\n" for request in requests),
                             text=True, capture_output=True, timeout=30)
    assert process.returncode == 0, process.stderr
    responses = [json.loads(line) for line in process.stdout.splitlines()]
    assert len(responses) == 4 and [response["ok"] for response in responses] == [True, False, True, True]
    assert responses[2]["result"]["dirty"] and responses[3]["result"]["closed"]

print("Editor protocol recovery, persistence, simulation, protected close and EOF teardown errors passed")
