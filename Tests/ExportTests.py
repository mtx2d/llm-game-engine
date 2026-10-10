"""Package a real game, move it, remove source assets, and run from another cwd."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
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


def package_resources(package):
    return package / "AsterGame.app/Contents/Resources" if sys.platform == "darwin" else package


def package_executable(package):
    if sys.platform == "darwin":
        return package / "AsterGame.app/Contents/MacOS/AsterGame"
    return package / ("AsterGame.exe" if os.name == "nt" else "AsterGame")


def macos_shipping_environment():
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("DYLD_", "VK_", "MVK_")) and
                   key not in ("VULKAN_SDK", "CMAKE_PREFIX_PATH")}
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    return environment


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
        expected_assets = {name: digest for name, digest in digest_tree(source).items()
                           if not any(part.lower() == ".aster" for part in Path(name).parts)}
        # Private/recovery files may be invalid assets or links. They must never
        # be traversed, validated or copied into a shipping package.
        for relative in (".aster", "Scenes/.ASTER"):
            private = source / relative
            private.mkdir(exist_ok=True)
            (private / "Invalid.aster").write_text("private unsaved work, not a game scene")
            (private / "Owner.lock").write_text("stable lock identity")
        try:
            (source / "PrivateAlias").symlink_to(source / ".aster", target_is_directory=True)
        except OSError:
            if os.name != "nt":
                raise
        output = root / "Game"
        output.mkdir()  # An existing empty destination is supported.
        response = export(args.editor, args.runtime, args.notices, source, output)
        check(response["ok"], f"Export failed: {response}")
        resources = package_resources(output)
        expected_notices = digest_tree(args.notices)
        macos_vulkan = False
        if sys.platform == "darwin":
            contents = output / "AsterGame.app/Contents"
            info = plistlib.loads((contents / "Info.plist").read_bytes())
            check(info["CFBundleExecutable"] == "AsterGame" and info["CFBundlePackageType"] == "APPL",
                  "Export did not produce an application bundle")
            imports = subprocess.check_output(["/usr/bin/otool", "-L", str(args.runtime)], text=True)
            macos_vulkan = "@rpath/libvulkan.1.dylib" in imports
            if macos_vulkan:
                companions = args.runtime.parent / "AsterRuntimeDependencies"
                for name in ("libvulkan.1.dylib", "libMoltenVK.dylib"):
                    library = contents / "Frameworks" / name
                    check(library.read_bytes() == (companions / name).read_bytes(),
                          f"Export omitted or changed macOS companion {name}")
                    libraries = subprocess.check_output(["/usr/bin/otool", "-L", str(library)], text=True)
                    for line in libraries.splitlines()[1:]:
                        if not line[:1].isspace():
                            continue  # A universal dylib prints a heading for each architecture.
                        dependency = line.strip().split(" (", 1)[0]
                        check(dependency.startswith(("@rpath/", "/usr/lib/", "/System/Library/")),
                              f"Bundled library retains a development dependency: {dependency}")
                license_bytes = (companions / "MoltenVK-LICENSE.txt").read_bytes()
                expected_notices["Licenses/MoltenVK.txt"] = hashlib.sha256(license_bytes).hexdigest()
                driver = json.loads((resources / "vulkan/icd.d/MoltenVK_icd.json").read_text())
                check(driver["ICD"]["is_portability_driver"] is True and
                      driver["ICD"]["library_path"] == "../../../Frameworks/libMoltenVK.dylib",
                      "Bundled MoltenVK manifest is not portable")
                load_commands = subprocess.check_output(["/usr/bin/otool", "-l", str(package_executable(output))], text=True)
                check("path @executable_path/../Frameworks " in load_commands,
                      "Exported runtime cannot locate its bundled libraries")
            else:
                check(not (contents / "Frameworks").exists(), "CPU-only bundle unexpectedly added graphics libraries")
        check(digest_tree(resources / "Assets") == expected_assets, "Export changed or omitted asset bytes")
        check(digest_tree(resources / "ThirdParty") == expected_notices, "Missing third-party notices")
        manifest = json.loads((resources / "Game.json").read_text())
        check(manifest == {"Version": 1, "Scene": "Scenes/FeatureGallery.aster", "Assets": "Assets"},
              "Manifest must contain portable relative paths")
        check(not any(path.is_symlink() for path in output.rglob("*")), "Package contains source links")
        check(not (output / args.editor.name).exists(), "Editor executable was included in runtime package")
        if os.name == "nt":
            companion_loader = args.runtime.parent / "vulkan-1.dll"
            exported_loader = output / "vulkan-1.dll"
            if companion_loader.exists():
                check(exported_loader.is_file() and exported_loader.read_bytes() == companion_loader.read_bytes(),
                      "Export omitted or changed the application-local Vulkan loader")
                check((output / "ThirdParty/Licenses/vulkan_loader.txt").is_file() and
                      (output / "ThirdParty/Licenses/vulkan_loader_notices.txt").is_file(),
                      "Exported Vulkan loader is missing its third-party license")
            else:
                check(not exported_loader.exists(), "Export unexpectedly added a Vulkan loader")

        moved = root / "Moved Game With Spaces"
        output.rename(moved)
        shutil.rmtree(source)
        elsewhere = root / "Elsewhere"
        elsewhere.mkdir()
        executable = package_executable(moved)
        resources = package_resources(moved)
        manifest_path = resources / "Game.json"
        state = elsewhere / "State.aster"
        process = subprocess.run(
            [str(executable), "--steps", "240", "--output", str(state)],
            cwd=elsewhere,
            text=True,
            capture_output=True,
            timeout=30,
            env=macos_shipping_environment() if sys.platform == "darwin" else None,
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
            shipping_environment = macos_shipping_environment() if sys.platform == "darwin" else os.environ.copy()
            for variable in ("LD_LIBRARY_PATH", "CMAKE_PREFIX_PATH", "VK_ADD_LAYER_PATH", "VK_INSTANCE_LAYERS",
                             "VK_LOADER_LAYERS_ENABLE"):
                shipping_environment.pop(variable, None)
            no_layers = root / "NoDevelopmentLayers"
            no_layers.mkdir()
            shipping_environment["VK_LAYER_PATH"] = str(no_layers)
            shipping_environment["VK_LOADER_LAYERS_DISABLE"] = "*validation*"
            if os.name == "nt":
                # The package must find its companion loader and normal OS/VC
                # runtime libraries without inheriting SDK or build-tool paths.
                system_root = Path(os.environ["SystemRoot"])
                shipping_environment["PATH"] = os.pathsep.join(str(path) for path in
                    (system_root / "System32", system_root, system_root / "System32/Wbem"))
                for variable in ("VULKAN_SDK", "VK_SDK_PATH", "ASTER_VULKAN_LOADER"):
                    shipping_environment.pop(variable, None)
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
        good_manifest = manifest_path.read_text()
        for field, value in (("Version", 99), ("Assets", "../Elsewhere"), ("Scene", "../escape.aster")):
            invalid = json.loads(good_manifest)
            invalid[field] = value
            manifest_path.write_text(json.dumps(invalid))
            process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                     capture_output=True, timeout=15)
            check(process.returncode != 0, f"Runtime accepted invalid manifest {field}")
        manifest_path.write_text(good_manifest)

        if sys.platform == "darwin":
            preserved_resources = root / "Preserved Resources"
            resources.rename(preserved_resources)
            resources.symlink_to(preserved_resources, target_is_directory=True)
            try:
                process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                         capture_output=True, timeout=15)
                check(process.returncode != 0 and "Resources must not be a symbolic link" in process.stderr,
                      "Runtime accepted a bundle Resources link escaping the application")
            finally:
                resources.unlink()
                preserved_resources.rename(resources)

        external_assets = root / "External Assets"
        shutil.copytree(args.assets, external_assets)
        assets_link = resources / "LinkedAssets"
        try:
            assets_link.symlink_to(external_assets, target_is_directory=True)
        except OSError as error:
            if os.name != "nt":
                raise
            print(f"Windows symlink creation unavailable; manifest symlink cases unverified: {error}")
        else:
            invalid = json.loads(good_manifest)
            invalid["Assets"] = "LinkedAssets"
            manifest_path.write_text(json.dumps(invalid))
            process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                     capture_output=True, timeout=15)
            check(process.returncode != 0, "Runtime accepted assets symlink escaping package")
            assets_link.unlink()
            scene_link = resources / "Assets/LinkedScene.aster"
            scene_link.symlink_to(external_assets / "Scenes/FeatureGallery.aster")
            invalid = json.loads(good_manifest)
            invalid["Scene"] = "LinkedScene.aster"
            manifest_path.write_text(json.dumps(invalid))
            process = subprocess.run([str(executable), "--steps", "1"], cwd=elsewhere, text=True,
                                     capture_output=True, timeout=15)
            check(process.returncode != 0, "Runtime accepted scene symlink escaping assets")
            scene_link.unlink()
            manifest_path.write_text(good_manifest)

        shutil.copytree(args.assets, source)
        if sys.platform == "darwin":
            runtime_fixture = root / "Mac Runtime Companion Failure"
            runtime_fixture.mkdir()
            fixture_executable = runtime_fixture / args.runtime.name
            shutil.copy2(args.runtime, fixture_executable)
            mac_failure = root / "Mac Failure"

            def reject_mac_runtime(reason):
                response = export(args.editor, fixture_executable, args.notices, source, mac_failure)
                check(not response["ok"] and not mac_failure.exists(), f"Exporter accepted {reason}")
                check(not list(root.glob("*.aster-export-*")), "Rejected macOS runtime left a staged export")

            if macos_vulkan:
                reject_mac_runtime("a graphical runtime without its companion directory")
                fixture_companions = runtime_fixture / "AsterRuntimeDependencies"
                shutil.copytree(args.runtime.parent / "AsterRuntimeDependencies", fixture_companions)
                for name in ("libvulkan.1.dylib", "libMoltenVK.dylib", "MoltenVK-LICENSE.txt", "MoltenVK_icd.json"):
                    companion = fixture_companions / name
                    original = companion.read_bytes()
                    companion.unlink()
                    reject_mac_runtime(f"missing companion {name}")
                    companion.write_bytes(b"")
                    reject_mac_runtime(f"empty companion {name}")
                    companion.unlink()
                    companion.mkdir()
                    reject_mac_runtime(f"directory companion {name}")
                    companion.rmdir()
                    companion.symlink_to(args.runtime.parent / "AsterRuntimeDependencies" / name)
                    reject_mac_runtime(f"symbolic-link companion {name}")
                    companion.unlink()
                    companion.write_bytes(original)
                driver_path = fixture_companions / "MoltenVK_icd.json"
                original_driver = json.loads(driver_path.read_text())
                for version in ("", None, 1.3, True, "1.2.999", "1.3", "1.3.0.0", "1..0", "-1.3.0",
                                "1.3.0junk", "128.3.0", "1.1024.0", "1.3.4096"):
                    invalid_driver = json.loads(json.dumps(original_driver))
                    invalid_driver["ICD"]["api_version"] = version
                    driver_path.write_text(json.dumps(invalid_driver))
                    reject_mac_runtime(f"invalid driver API version {version!r}")
                invalid_driver = json.loads(json.dumps(original_driver))
                invalid_driver["file_format_version"] = "99.0.0"
                driver_path.write_text(json.dumps(invalid_driver))
                reject_mac_runtime("unsupported driver manifest format version")
                driver_path.write_text('{"file_format_version":"1.0.0","ICD":{"is_portability_driver":false}}')
                reject_mac_runtime("a malformed MoltenVK portability manifest")
            fixture_executable.write_bytes(b"invalid Mach-O executable")
            reject_mac_runtime("an invalid Mach-O runtime")
        if os.name == "nt":
            runtime_fixture = root / "Runtime Companion Failure"
            runtime_fixture.mkdir()
            fixture_executable = runtime_fixture / args.runtime.name
            shutil.copy2(args.runtime, fixture_executable)
            bad_loader = runtime_fixture / "vulkan-1.dll"
            loader_failure = root / "Loader Failure"
            bad_loader.mkdir()
            response = export(args.editor, fixture_executable, args.notices, source, loader_failure)
            check(not response["ok"] and not loader_failure.exists(),
                  "Exporter accepted a directory instead of its Vulkan loader")
            bad_loader.rmdir()
            bad_loader.write_bytes(b"")
            response = export(args.editor, fixture_executable, args.notices, source, loader_failure)
            check(not response["ok"] and not loader_failure.exists(),
                  "Exporter accepted an empty Vulkan loader")
            check(not list(root.glob("*.aster-export-*")), "Rejected loader left a staged export")
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
        private = source / ".aster"
        private.mkdir(exist_ok=True)
        (private / "Private.bin").write_bytes(b"private asset data")
        for uri in (".aster/Private.bin", "%2easter/Private.bin", ".ASTER/Private.bin"):
            gltf.write_text(json.dumps({"asset": {"version": "2.0"}, "buffers": [{"uri": uri, "byteLength": 18}]}))
            response = export(args.editor, args.runtime, args.notices, source, failed)
            check(not response["ok"] and "reserved editor storage" in response["error"] and not failed.exists(),
                  f"Exporter did not reject private metadata URI {uri}: {response}")
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
