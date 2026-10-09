"""Package a real game, move it, remove source assets, and run from another cwd."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def export(editor, runtime, notices, assets, output, scene="Scenes/FeatureGallery.aster"):
    request = {
        "command": "project.export",
        "scene": scene,
        "runtime": str(runtime),
        "notices": str(notices),
        "output": str(output),
    }
    process = subprocess.run(
        [str(editor), "--automation", str(assets)],
        input=json.dumps(request) + "\n",
        text=True,
        capture_output=True,
        timeout=30,
        check=True,
    )
    lines = process.stdout.splitlines()
    check(len(lines) == 1, f"Expected one exporter response: {process.stdout}")
    return json.loads(lines[0])


def digest_tree(directory):
    return {
        path.relative_to(directory).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in directory.rglob("*")
        if path.is_file()
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    parser.add_argument("--graphical", action="store_true")
    args = parser.parse_args()
    args.editor = args.editor.resolve()
    args.runtime = args.runtime.resolve()
    args.assets = args.assets.resolve()
    args.notices = args.notices.resolve()

    with tempfile.TemporaryDirectory(prefix="aster-export-") as temporary:
        root = Path(temporary)
        source = root / "Source Assets"
        shutil.copytree(args.assets, source)
        expected_assets = digest_tree(source)
        output = root / "Game"
        output.mkdir()  # An existing empty destination is supported.
        response = export(args.editor, args.runtime, args.notices, source, output)
        check(response["ok"], f"Export failed: {response}")
        check(digest_tree(output / "Assets") == expected_assets, "Export changed or omitted asset bytes")
        check(digest_tree(output / "ThirdParty") == digest_tree(args.notices), "Missing third-party notices")
        manifest = json.loads((output / "Game.json").read_text())
        check(manifest == {"Version": 1, "Scene": "Scenes/FeatureGallery.aster", "Assets": "Assets"},
              "Manifest must contain portable relative paths")
        check(not any(path.is_symlink() for path in output.rglob("*")), "Package contains source links")
        check(not (output / args.editor.name).exists(), "Editor executable was included in runtime package")

        moved = root / "Moved Game With Spaces"
        output.rename(moved)
        shutil.rmtree(source)
        elsewhere = root / "Elsewhere"
        elsewhere.mkdir()
        executable = moved / ("AsterGame.exe" if os.name == "nt" else "AsterGame")
        state = elsewhere / "State.aster"
        process = subprocess.run(
            [str(executable), "--steps", "240", "--output", str(state)],
            cwd=elsewhere,
            text=True,
            capture_output=True,
            timeout=30,
        )
        check(process.returncode == 0, f"Relocated game failed: {process.stdout}\n{process.stderr}")
        result = json.loads(process.stdout)
        check(result["errors"] == [] and result["steps"] == 240, "Exported simulation reported an error")
        check(any("all 32" in line for line in result["log"]), "Exported game did not execute feature Lua")
        for event in ("capsule trigger entered", "capsule trigger exited", "trigger lifecycle released"):
            check(f"FeatureGallery: {event}" in result["log"], f"Exported trigger callback missing: {event}")
        entities = json.loads(state.read_text())["Entities"]
        sphere = next(entity for entity in entities if entity["Name"] == "FallingSphere")
        check(abs(sphere["Transform"]["Translation"][1] - 0.5) < 0.02,
              "Exported real physics failed to settle sphere on ground")
        gallery = next(entity for entity in entities if entity["Name"] == "Gallery")
        check(gallery["Transform"]["Rotation"][1] > 0.9, "Exported Lua did not update scene transforms")
        platform = next(entity for entity in entities if entity["Name"] == "KinematicPlatform")
        check(platform["RigidBody"]["Type"] == "Kinematic" and platform["Transform"]["Translation"][1] < 0.3,
              "Exported kinematic platform did not follow its scripted motion")
        check({entity["Light"]["Type"] for entity in entities if "Light" in entity} ==
              {"Directional", "Point", "Spot"}, "Feature scene must author every light type")
        check({entity["RigidBody"]["Shape"] for entity in entities if "RigidBody" in entity} ==
              {"Box", "Sphere", "Capsule"}, "Feature scene must author every collision shape")
        check({entity["RigidBody"]["Type"] for entity in entities if "RigidBody" in entity} ==
              {"Static", "Kinematic", "Dynamic"}, "Feature scene must author every body type")

        if args.graphical:
            shipping_environment = os.environ.copy()
            for variable in ("LD_LIBRARY_PATH", "CMAKE_PREFIX_PATH", "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS",
                             "VK_LOADER_LAYERS_ENABLE"):
                shipping_environment.pop(variable, None)
            no_layers = root / "NoDevelopmentLayers"
            no_layers.mkdir()
            shipping_environment["VK_LAYER_PATH"] = str(no_layers)
            shipping_environment["VK_LOADER_LAYERS_DISABLE"] = "*validation*"
            process = subprocess.run([str(executable), "--window", "--steps", "5"], cwd=elsewhere,
                                     env=shipping_environment, text=True, capture_output=True, timeout=45)
            check(process.returncode == 0, f"Graphical shipping game required development paths/layers: {process.stdout}\n{process.stderr}")
            result = json.loads(process.stdout)
            check(result["errors"] == [] and result["frames"] == result["steps"] == 5,
                  "Relocated graphical game did not advance and present five frames")
            process = subprocess.run([str(executable), "--window", "--steps", "5", "--validation"],
                                     cwd=elsewhere, text=True, capture_output=True, timeout=45)
            check(process.returncode == 0, f"Relocated graphical validation failed: {process.stdout}\n{process.stderr}")
            check(json.loads(process.stdout)["errors"] == [], "Graphical validation run reported script errors")

        # Manifest failures must not silently fall back to the process cwd.
        good_manifest = (moved / "Game.json").read_text()
        for field, value in (("Version", 99), ("Assets", "../Elsewhere"), ("Scene", "../escape.aster")):
            invalid = json.loads(good_manifest)
            invalid[field] = value
            (moved / "Game.json").write_text(json.dumps(invalid))
            process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                     capture_output=True, timeout=15)
            check(process.returncode != 0, f"Runtime accepted invalid manifest {field}")
        (moved / "Game.json").write_text(good_manifest)

        external_assets = root / "External Assets"
        shutil.copytree(args.assets, external_assets)
        assets_link = moved / "LinkedAssets"
        try:
            assets_link.symlink_to(external_assets, target_is_directory=True)
        except OSError as error:
            if os.name != "nt":
                raise
            print(f"Windows symlink creation unavailable; manifest symlink cases unverified: {error}")
        else:
            invalid = json.loads(good_manifest)
            invalid["Assets"] = "LinkedAssets"
            (moved / "Game.json").write_text(json.dumps(invalid))
            process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                     capture_output=True, timeout=15)
            check(process.returncode != 0, "Runtime accepted assets symlink escaping package")
            assets_link.unlink()
            scene_link = moved / "Assets/LinkedScene.aster"
            scene_link.symlink_to(external_assets / "Scenes/FeatureGallery.aster")
            invalid = json.loads(good_manifest)
            invalid["Scene"] = "LinkedScene.aster"
            (moved / "Game.json").write_text(json.dumps(invalid))
            process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                     capture_output=True, timeout=15)
            check(process.returncode != 0, "Runtime accepted scene symlink escaping assets")
            scene_link.unlink()
            (moved / "Game.json").write_text(good_manifest)

        shutil.copytree(args.assets, source)
        occupied = root / "Occupied"
        occupied.mkdir()
        (occupied / "UserData.txt").write_text("keep me")
        response = export(args.editor, args.runtime, args.notices, source, occupied)
        check(not response["ok"] and (occupied / "UserData.txt").read_text() == "keep me",
              "Exporter overwrote an occupied destination")
        response = export(args.editor, args.runtime, args.notices, source, source / "Nested Output")
        check(not response["ok"] and not (source / "Nested Output").exists(), "Exporter recursed into source tree")
        failed = root / "Failed Export"

        def reject_current_assets(reason):
            response = export(args.editor, args.runtime, args.notices, source, failed)
            check(not response["ok"], f"Exporter accepted {reason}")
            check(not failed.exists(), f"Failed export left output for {reason}")
            check(not list(root.glob("*.aster-export-*")), f"Failed export leaked staging for {reason}")

        scene_path = source / "Scenes/FeatureGallery.aster"
        good_scene = scene_path.read_text()
        scene = json.loads(good_scene)
        scene["Entities"][0]["Script"]["Path"] = "Scripts/Missing.lua"
        scene_path.write_text(json.dumps(scene))
        reject_current_assets("missing scene asset")
        scene_path.write_text(good_scene)

        # Existence and URI checks alone cannot establish that a referenced mesh
        # will load in the exported runtime. Exercise the actual importer gate.
        authored_scene = json.loads(good_scene)
        mesh_reference = next(entity["MeshRenderer"]["Mesh"] for entity in authored_scene["Entities"]
                              if "MeshRenderer" in entity)
        mesh_path = source / mesh_reference
        good_mesh = mesh_path.read_text()
        malformed_mesh = json.loads(good_mesh)
        malformed_mesh["accessors"][0]["count"] = 9999999
        mesh_path.write_text(json.dumps(malformed_mesh))
        reject_current_assets("referenced glTF accessor extending beyond its buffer")
        mesh_path.write_text(good_mesh)

        environment_path = source / authored_scene["Environment"]["Path"]
        good_environment = environment_path.read_bytes()
        environment_path.write_bytes(b"This is not a Radiance HDR image")
        reject_current_assets("malformed referenced environment image")
        environment_path.write_bytes(good_environment)

        response = export(args.editor, args.runtime, args.notices, source, failed, "../escape.aster")
        check(not response["ok"], "Exporter accepted scene traversal")

        (root / "Outside.bin").write_bytes(b"outside asset root")
        gltf = source / "Invalid.gltf"
        for uri in ("Missing.bin", "%2e%2e/Outside.bin", "https://example.invalid/remote.bin"):
            gltf.write_text(json.dumps({"asset": {"version": "2.0"}, "buffers": [{"uri": uri, "byteLength": 18}]}))
            reject_current_assets(f"glTF URI {uri}")
        gltf.unlink()
        glb = source / "Invalid.glb"
        glb.write_bytes(b"glTF\x02\x00\x00\x00")
        reject_current_assets("truncated GLB")
        glb.unlink()

        link = source / "Escaping.bin"
        try:
            link.symlink_to(root / "Outside.bin")
        except OSError as error:
            if os.name != "nt":
                raise
            print(f"Windows symlink creation unavailable; symlink cases unverified: {error}")
        else:
            reject_current_assets("escaping asset symlink")
            link.unlink()
            loop = source / "Cycle"
            loop.symlink_to(source, target_is_directory=True)
            reject_current_assets("cyclic directory symlink")
            loop.unlink()

        print("Export: relocated runtime, scene physics/Lua/audio, asset bytes, notices, and failure paths passed")


if __name__ == "__main__":
    main()
