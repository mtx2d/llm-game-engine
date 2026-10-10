"""Create, author, relocate and export a project through real application processes."""

import argparse
import json
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading

from ExportTests import package_executable, package_resources, macos_shipping_environment
from EditorRenderTests import read_image, region
from CreateBlockStack import author
from RecoveryTests import Editor as AutomationEditor


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    parser.add_argument("--graphical", action="store_true")
    args = parser.parse_args()
    editor, runtime, assets, notices = (path.resolve() for path in
                                       (args.editor, args.runtime, args.assets, args.notices))
    with tempfile.TemporaryDirectory(prefix="AsterProjectWorkflow-") as temporary:
        root = Path(temporary).resolve()
        cwd = root / "Unrelated working directory"
        cwd.mkdir()

        def run(arguments, expected=0, **kwargs):
            result = subprocess.run(list(map(str, arguments)), cwd=cwd, text=True,
                                    capture_output=True, timeout=45, **kwargs)
            assert result.returncode == expected, result.stdout + result.stderr
            return result

        def automate(project, requests):
            result = run([editor, "--automation", project],
                         input="".join(json.dumps(request) + "\n" for request in requests))
            responses = [json.loads(line) for line in result.stdout.splitlines()]
            assert len(responses) == len(requests), result.stdout
            return responses

        source = root / "New game with spaces"
        run([runtime, "--project", root / "Missing.asterproj", "--steps", 0], expected=1)
        created = json.loads(run([editor, "--create-project", source, "Production workflow"]).stdout)
        manifest = Path(created["project"])
        assert manifest == source / "Project.asterproj" and manifest.is_file()
        original = manifest.read_bytes()
        run([editor, "--create-project", source, "Overwrite"], expected=1)
        assert manifest.read_bytes() == original, "Duplicate creation changed the project"

        conflict_root = root / "Concurrent editing"
        conflict_project = Path(json.loads(run(
            [editor, "--create-project", conflict_root, "Conflicts"]).stdout)["project"])
        conflict_scene = conflict_root / "Assets/Scenes/Main.aster"
        with (root / "LiveEditor.stderr").open("w+") as diagnostics:
            live = subprocess.Popen([str(editor), "--automation", str(conflict_project)], cwd=cwd,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=diagnostics,
                                    text=True)
            replies = queue.Queue()

            def read_replies():
                for line in live.stdout:
                    replies.put(line)
                replies.put(None)

            reader = threading.Thread(target=read_replies, daemon=True)
            reader.start()
            try:
                def send(request):
                    live.stdin.write(json.dumps(request) + "\n")
                    live.stdin.flush()
                    line = replies.get(timeout=15)
                    assert line is not None, "Live editor exited without its command response"
                    return json.loads(line)

                assert send({"command": "entity.create", "name": "Pending live edit"})["ok"]
                changed = automate(conflict_project, [
                    {"command": "entity.create", "name": "External persisted edit"},
                    {"command": "scene.save", "path": "Scenes/Main.aster"}])
                assert all(response["ok"] for response in changed), changed
                external_bytes = conflict_scene.read_bytes()
                rejected = send({"command": "scene.save", "path": "Scenes/Main.aster"})
                assert not rejected["ok"] and "conflict" in rejected["error"], rejected
                assert conflict_scene.read_bytes() == external_bytes
                assert send({"command": "scene.status"})["result"]["dirty"]
                assert send({"command": "scene.get"})["result"]["Entities"][1]["Name"] == "Pending live edit"
                assert not send({"command": "session.close"})["ok"], "Conflict must not permit dirty close"
                assert send({"command": "scene.save", "path": "Scenes/LiveRecovered.aster"})["ok"]
                assert send({"command": "session.close"})["result"]["closed"]
                live.stdin.close()
                assert live.wait(timeout=15) == 0
                recovered = json.loads((conflict_root / "Assets/Scenes/LiveRecovered.aster").read_text())
                assert recovered["Entities"][1]["Name"] == "Pending live edit"
                assert conflict_scene.read_bytes() == external_bytes, "Recovery overwrote external work"
            finally:
                if live.poll() is None:
                    live.terminate()
                    live.wait(timeout=5)
                reader.join(timeout=5)
                assert not reader.is_alive(), "Live editor protocol reader failed to stop"
                live.stdout.close()
                if not live.stdin.closed:
                    live.stdin.close()

        config_root = root / "Configuration workflow"
        config_project = Path(json.loads(run(
            [editor, "--create-project", config_root, "Configuration owners"]).stdout)["project"])
        with AutomationEditor(editor, config_project) as first, AutomationEditor(editor, config_project) as second:
            first_config = first.get("project.get")["config"]
            second.get("entity.create", name="Pending during configuration conflict")
            pending = second.get("scene.get")
            first_config["Name"] = "First published configuration"
            assert first.get("project.configure", config=first_config)["documentChanged"] is False
            external_bytes = config_project.read_bytes()
            first_config["Name"] = "Stale second configuration"
            rejected = second.request("project.configure", config=first_config)
            assert not rejected["ok"] and "conflict" in rejected["error"], rejected
            assert config_project.read_bytes() == external_bytes
            assert second.get("scene.get") == pending and second.get("scene.status")["dirty"]
            assert not second.request("project.export")["ok"], "Dirty/conflicting export bypassed validation"
            second.get("scene.save", path="Scenes/PreservedPending.aster")
            rejected_export = second.request("project.export")
            assert not rejected_export["ok"] and "Project save conflict" in rejected_export["error"], rejected_export
            second.get("project.open", path=str(config_project))
            assert second.get("project.get")["config"]["Name"] == "First published configuration"
            assert not second.request("history.undo")["ok"]
            second.get("project.configure", config=first_config)
            assert json.loads(config_project.read_text())["Name"] == "Stale second configuration"
            assert json.loads((config_root / "Assets/Scenes/PreservedPending.aster").read_text()) == pending
        new_assets = config_root / "AlternateAssets"
        shutil.copytree(config_root / "Assets", new_assets)
        alternate = new_assets / "Scenes/Main.aster"
        alternate_scene = json.loads(alternate.read_text())
        alternate_scene["Name"] = "Alternate configured startup"
        alternate.write_text(json.dumps(alternate_scene))
        with AutomationEditor(editor, config_project) as changing:
            config = changing.get("project.get")["config"]
            config["AssetDirectory"] = "AlternateAssets"
            result = changing.get("project.configure", config=config)
            assert result["documentChanged"] is True and result["warning"] is None
            assert changing.get("scene.get") == alternate_scene and not changing.get("scene.status")["dirty"]
            assert Path(changing.get("project.get")["assets"]) == new_assets
        configured_output = root / "ConfiguredRuntime.aster"
        run([runtime, "--project", config_project, "--steps", 0, "--output", configured_output])
        assert json.loads(configured_output.read_text()) == alternate_scene

        repair_root = root / "Repair startup workflow"
        run([editor, "--create-project", repair_root, "Repair workflow"])
        repair_project = repair_root / "Project.asterproj"
        repair_assets = repair_root / "Assets"
        bad_startup = repair_assets / "Scenes/Main.aster"
        with AutomationEditor(editor, repair_project) as before_crash:
            before_crash.get("entity.create", name="Recover across corrupt startup")
            pending_repair = before_crash.get("scene.get")
            before_crash.get("recovery.checkpoint")
            repair_session = before_crash.get("recovery.list")[0]["id"]
            before_crash.kill()
        bad_startup.write_bytes(b"Corrupt startup must be preserved")
        unchanged_manifest = repair_project.read_bytes()
        run([runtime, "--project", repair_project, "--steps", 0], expected=1)
        run([editor, "--automation", repair_project], expected=1)
        with AutomationEditor(editor, repair_assets) as repairing:
            rejected = repairing.request("project.open", path=str(repair_project), repairStartup="true")
            assert not rejected["ok"] and repairing.get("project.get")["path"] is None
            opened = repairing.get("project.open", path=str(repair_project), repairStartup=True)
            assert opened["warning"] and repairing.get("project.get")["startupError"]
            assert repairing.get("scene.status")["path"] is None and not repairing.get("scene.status")["dirty"]
            assert not repairing.request("simulation.start", audio="offline")["ok"]
            assert not repairing.request("project.export")["ok"]
            adopted = repairing.get("recovery.restore", session=repair_session)
            assert adopted["path"] is None and adopted["warning"]
            assert repairing.get("scene.get") == pending_repair
            assert not repairing.request("scene.save", path="Scenes/Main.aster")["ok"]
            assert bad_startup.read_bytes() == b"Corrupt startup must be preserved"
            assert repair_project.read_bytes() == unchanged_manifest
            repairing.get("scene.save", path="Scenes/Recovered.aster")
            repair_config = repairing.get("project.get")["config"]
            repair_config["StartScene"] = "Scenes/Recovered.aster"
            assert repairing.get("project.configure", config=repair_config)["documentChanged"]
            assert repairing.get("project.get")["startupError"] is None
            assert repairing.get("scene.get") == pending_repair
            repairing.get("simulation.start", audio="offline")
            repairing.get("simulation.stop")
        repaired_output = root / "RepairedRuntime.aster"
        run([runtime, "--project", repair_project, "--steps", 0, "--output", repaired_output])
        assert json.loads(repaired_output.read_text()) == pending_repair
        assert bad_startup.read_bytes() == b"Corrupt startup must be preserved"

        editor_state = root / "Shared editor state"
        editor_state.mkdir()
        with AutomationEditor(editor, repair_assets, editor_state) as first_history, \
             AutomationEditor(editor, repair_assets, editor_state) as second_history:
            first_history.get("project.open", path=str(repair_project))
            second_history.get("project.open", path=str(manifest))
            recent = first_history.get("project.recent")
            assert recent == {"enabled": True, "projects": [manifest.as_posix(), repair_project.as_posix()]}
            before_rejection = (editor_state / ".aster/Projects.json").read_bytes()
            assert not first_history.request("project.open", path=str(root / "Missing.asterproj"))["ok"]
            assert (editor_state / ".aster/Projects.json").read_bytes() == before_rejection
            second_history.get("project.forget", path=str(manifest))
            assert first_history.get("project.recent")["projects"] == [repair_project.as_posix()]
            assert manifest.read_bytes() == original, "Forgetting history changed the project file"
        with AutomationEditor(editor, repair_assets, editor_state) as restarted_history:
            assert {"project.browse", "project.recent", "project.forget"} <= set(restarted_history.get("help"))
            assert restarted_history.get("project.recent")["projects"] == [repair_project.as_posix()]
            browser = restarted_history.get("project.browse", path=str(source))
            assert any(entry["path"] == manifest.as_posix() and not entry["directory"] for entry in browser["entries"])

        (source / "Assets/Scripts").mkdir()
        (source / "Assets/Scripts/Actor.lua").write_text(
            'return {OnCreate=function(self, entity) '
            'engine.set_name(entity, "Project actor started") '
            'engine.set_position(entity, 1, 2, 0) '
            'engine.log("Project script resolved from asset root") end}', encoding="utf-8")
        (source / "Assets/Models").mkdir()
        shutil.copyfile(assets / "Models/Cube.gltf", source / "Assets/Models/Cube.gltf")
        author(editor, source / "Assets", "Authored prefab", "Scenes/AuthoredPrefab.json", prefab=True)
        prefab_bytes = (source / "Assets/Scenes/AuthoredPrefab.json").read_bytes()
        author(editor, source / "Assets", "Authored prefab", "Scenes/AuthoredPrefab.json", prefab=True)
        assert (source / "Assets/Scenes/AuthoredPrefab.json").read_bytes() == prefab_bytes, \
            "Explicit tool reauthoring changed the deterministic prefab"
        responses = automate(manifest, [
            {"command": "project.get"},
            {"command": "scene.status"},
            {"command": "entity.create", "name": "Actor"},
            {"command": "entity.patch", "entity": 2, "patch": {
                "Script": {"Path": "Scripts/Actor.lua", "Enabled": True},
                "Light": {"Type": "Directional", "Color": [1, 1, 1], "Intensity": 4,
                          "Range": 30, "InnerCone": 20, "OuterCone": 30, "CastShadows": False},
                "MeshRenderer": {"Mesh": "Models/Cube.gltf", "BaseColor": [0.9, 0.2, 0.1, 1],
                                 "Metallic": 0, "Roughness": 0.5, "Visible": True}}},
            {"command": "scene.load", "path": "Scenes/Main.aster"},
            {"command": "scene.status"},
            {"command": "scene.save", "path": "Scenes/Main.aster"},
            {"command": "scene.status"},
        ])
        assert [response["ok"] for response in responses] == [True, True, True, True, False, True, True, True]
        assert responses[0]["result"]["config"]["Name"] == "Production workflow"
        assert responses[1]["result"]["dirty"] is False
        assert responses[5]["result"]["dirty"] is True and responses[7]["result"]["dirty"] is False
        authored = (source / "Assets/Scenes/Main.aster").read_bytes()
        second = root / "Second project"
        switched = automate(manifest, [
            {"command": "project.create", "path": str(second), "name": "Created through automation"},
            {"command": "project.get"}, {"command": "scene.get"},
            {"command": "project.open", "path": str(manifest)}, {"command": "scene.get"},
        ])
        assert all(response["ok"] for response in switched), switched
        assert switched[1]["result"]["config"]["Name"] == "Created through automation"
        assert len(switched[2]["result"]["Entities"]) == 1
        assert len(switched[4]["result"]["Entities"]) == 2, "Opening original project did not restore its startup scene"
        output = root / "State.aster"

        def check_runtime(project, scene_override=None):
            arguments = [runtime, "--project", project, "--steps", 3, "--output", output]
            if scene_override:
                arguments += ["--scene", scene_override]
            result = json.loads(run(arguments).stdout)
            assert result["steps"] == 3 and result["errors"] == [] and result["entities"] == 2
            assert any("Project script resolved from asset root" in message for message in result["log"])
            scene = json.loads(output.read_text())
            actor = next(entity for entity in scene["Entities"] if entity["ID"] == 2)
            assert actor["Name"] == "Project actor started" and actor["Transform"]["Translation"] == [1, 2, 0]

        check_runtime(manifest)
        assert (source / "Assets/Scenes/Main.aster").read_bytes() == authored, "Play modified authored scene"
        relocated = root / "Relocated project"
        shutil.move(source, relocated)
        assert not source.exists()
        manifest = relocated / "Project.asterproj"
        check_runtime(manifest)
        check_runtime(manifest, "Scenes/Main.aster")
        run([runtime, "--project", manifest, "--scene", "../Escape.aster", "--steps", 0], expected=1)

        # The editor's active asset root must drive export, not the manifest directory or cwd.
        package = root / "Exported game"
        responses = automate(manifest, [{"command": "project.export", "scene": "Scenes/Main.aster",
                                        "runtime": str(runtime), "notices": str(notices), "output": str(package)}])
        assert responses[0]["ok"], responses
        shipped_manifest = package_resources(package) / "Game.json"
        assert (package_resources(package) / "Assets/Scripts/Actor.lua").is_file()
        result = json.loads(run([package_executable(package), "--steps", 3, "--output", output],
                                env=macos_shipping_environment()).stdout)
        assert result["errors"] == [] and result["entities"] == 2 and result["steps"] == 3
        assert any("Project script resolved from asset root" in message for message in result["log"])

        if args.graphical:
            screenshot = root / "ProjectEditor.ppm"
            run([editor, "--project", manifest, "--frames", 3, "--audio", "offline", "--screenshot", screenshot])
            image = read_image(screenshot)
            _, _, pixels = image
            assert len(set(zip(pixels[0::3], pixels[1::3], pixels[2::3]))) > 500, \
                "Project editor screenshot lacks rendered content"
            viewport = region(image, (270, 115, 1090, 600))
            red_pixels = sum(red > 40 and red > green * 1.5 and red > blue * 1.5
                             for red, green, blue in zip(viewport[0::3], viewport[1::3], viewport[2::3]))
            assert red_pixels > 20, f"Relocated project's authored red mesh was not rendered: {red_pixels} pixels"
            assert (relocated / "Assets/Scenes/Main.aster").read_bytes() == authored
            result = json.loads(run([runtime, "--project", manifest, "--steps", 3, "--window", "--validation"]).stdout)
            assert result["errors"] == [] and result["frames"] == 3

        original_game = shipped_manifest.read_bytes()
        shipped_manifest.write_text('{"Version":1,"Version":1,"Assets":"Assets","Scene":"Scenes/Main.aster"}',
                                    encoding="utf-8")
        result = run([package_executable(package), "--steps", 0], expected=1)
        assert "Duplicate JSON key" in result.stderr, result.stderr
        shipped_manifest.write_bytes(original_game)
        manifest.write_text('{"Version":1,"Name":"Bad","AssetDirectory":"../Escape",'
                            '"StartScene":"Scenes/Main.aster"}', encoding="utf-8")
        run([editor, "--automation", manifest], expected=1, input="")
        run([runtime, "--project", manifest, "--steps", 0], expected=1)
    print("Project creation, dirty documents, startup scene, scripts, relocation, export and invalid input passed")


if __name__ == "__main__":
    main()
