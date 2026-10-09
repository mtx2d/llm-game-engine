"""Recreate BlockStack scenes and prefab through Aster's shared authoring commands."""

import argparse
import json
from pathlib import Path
import subprocess


def author(editor, assets, scene_name, path, seeds=(), prefab=False):
    commands = [{"command": "scene.new", "name": scene_name}]
    next_id = 0

    def entity(name, patch, parent=None):
        nonlocal next_id
        next_id += 1
        commands.append({"command": "entity.create", "name": name})
        commands.append({"command": "entity.patch", "entity": next_id, "patch": patch})
        if parent is not None:
            commands.append({"command": "entity.parent", "entity": next_id, "parent": parent})
        return next_id

    def transform(position, scale=(1, 1, 1), rotation=(0, 0, 0)):
        return {"Translation": list(position), "Rotation": list(rotation), "Scale": list(scale)}

    def mesh(position, scale, color):
        return {
            "Transform": transform(position, scale),
            "MeshRenderer": {"Mesh": "Models/Cube.gltf", "BaseColor": list(color),
                             "Metallic": 0.05, "Roughness": 0.65, "Visible": True},
        }

    if prefab:
        entity("Block", mesh((0, 0, 0), (0.9, 0.9, 0.9), (0.5, 0.8, 1, 1)))
    else:
        game = entity("BlockStack", {"Script": {"Path": "Games/BlockStack/BlockStack.lua", "Enabled": True}})
        entity("Game camera", {"Transform": transform((2.3, -0.3, 33)),
                               "Camera": {"VerticalFov": 45, "NearClip": 0.1, "FarClip": 100, "Primary": True}})
        entity("Sun", {"Transform": transform((0, 10, 10), rotation=(-0.5, -0.4, 0)),
                       "Light": {"Type": "Directional", "Color": [1, 0.97, 0.91], "Intensity": 4,
                                 "Range": 30, "InnerCone": 20, "OuterCone": 30, "CastShadows": True}})
        entity("Board", mesh((0, 0, -0.75), (10.4, 20.4, 0.3), (0.025, 0.05, 0.075, 1)))
        for x in (-5.35, 5.35):
            entity("Board edge", mesh((x, 0, -0.2), (0.2, 20.8, 0.5), (0.13, 0.35, 0.55, 1)))
        for y in (-10.35, 10.35):
            entity("Board edge", mesh((0, y, -0.2), (10.9, 0.2, 0.5), (0.13, 0.35, 0.55, 1)))
        for x in range(-4, 5):
            entity("Grid", mesh((x, 0, -0.53), (0.018, 20, 0.02), (0.12, 0.2, 0.25, 1)))
        for y in range(-9, 10):
            entity("Grid", mesh((0, y, -0.53), (10, 0.018, 0.02), (0.12, 0.2, 0.25, 1)))
        for column, row in seeds:
            entity(f"Seed:{column}:{row}", {}, game)
        commands.append({"command": "scene.environment", "environment": {
            "Path": "Environment/StudioSmall09.hdr", "Intensity": 0.35, "Rotation": 1.7}})
    commands.append({"command": "scene.save", "path": path})
    process = subprocess.run([str(editor), "--automation", str(assets)],
                             input="".join(json.dumps(command) + "\n" for command in commands),
                             text=True, capture_output=True, timeout=60, check=True)
    responses = [json.loads(line) for line in process.stdout.splitlines()]
    if len(responses) != len(commands):
        raise RuntimeError(f"Incomplete authoring response: {process.stdout}\n{process.stderr}")
    for command, response in zip(commands, responses):
        if not response.get("ok"):
            raise RuntimeError(f"Authoring command failed: {command}: {response}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=Path("Assets"))
    args = parser.parse_args()
    assets = args.assets.resolve()
    editor = args.editor.resolve()
    (assets / "Games/BlockStack").mkdir(parents=True, exist_ok=True)
    author(editor, assets, "BlockStack Block", "Games/BlockStack/Block.prefab.json", prefab=True)
    author(editor, assets, "BlockStack", "Games/BlockStack/BlockStack.aster")
    seeds = [(column, row) for row in (0, 1) for column in range(10) if column not in (4, 5)]
    author(editor, assets, "BlockStack Line Clear", "Games/BlockStack/LineClear.aster", seeds)
    author(editor, assets, "BlockStack Top Out", "Games/BlockStack/TopOut.aster", [(4, 18), (5, 18), (4, 19), (5, 19)])
    print("BlockStack scenes and prefab authored through the command protocol")


if __name__ == "__main__":
    main()
