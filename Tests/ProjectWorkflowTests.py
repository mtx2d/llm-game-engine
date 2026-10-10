"""Create, author, relocate and export a project through real application processes."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from ExportTests import package_executable, package_resources, macos_shipping_environment


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

        (source / "Assets/Scripts").mkdir()
        (source / "Assets/Scripts/Actor.lua").write_text(
            'return {OnCreate=function(self, entity) '
            'engine.set_name(entity, "Project actor started") '
            'engine.set_position(entity, 1, 2, 0) '
            'engine.log("Project script resolved from asset root") end}', encoding="utf-8")
        (source / "Assets/Models").mkdir()
        shutil.copyfile(assets / "Models/Cube.gltf", source / "Assets/Models/Cube.gltf")
        responses = automate(manifest, [
            {"command": "project.get"},
            {"command": "scene.status"},
            {"command": "entity.create", "name": "Actor"},
            {"command": "entity.patch", "entity": 2, "patch": {
                "Script": {"Path": "Scripts/Actor.lua", "Enabled": True},
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
            assert screenshot.read_bytes().startswith(b"P6\n1440 900\n255\n")
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
