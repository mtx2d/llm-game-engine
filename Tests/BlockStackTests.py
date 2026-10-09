"""Exercise a complete authored game through the public command and input APIs."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

from ExportTests import package_executable


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(editor, assets, requests):
    process = subprocess.run([str(editor), "--automation", str(assets)],
                             input="".join(json.dumps(request) + "\n" for request in requests),
                             capture_output=True, text=True, timeout=60, check=True)
    responses = [json.loads(line) for line in process.stdout.splitlines()]
    check(len(responses) == len(requests), f"Incomplete command output: {process.stdout}\n{process.stderr}")
    for request, response in zip(requests, responses):
        check(response.get("ok"), f"Command failed: {request}: {response}")
        check(not isinstance(response["result"], dict) or not response["result"].get("errors"),
              f"Game script error: {response}")
    return [response["result"] for response in responses]


def state(scene):
    game = next(entity for entity in scene["Entities"] if entity.get("Script", {}).get("Path") == "Games/BlockStack/BlockStack.lua")
    values = dict(item.split("=", 1) for item in game["Name"].split("|")[1:])
    for key in ("score", "lines", "x", "y", "rotation"):
        values[key] = int(values[key])
    return values


def input_keys(*keys):
    return {"command": "input.set", "input": {"KeysDown": list(keys)}}


def step(count=1):
    return {"command": "simulation.step", "steps": count}


GET = {"command": "scene.get"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--notices", type=Path, required=True)
    args = parser.parse_args()
    editor, runtime, assets, notices = (path.resolve() for path in (args.editor, args.runtime, args.assets, args.notices))

    requests = [{"command": "scene.load", "path": "Games/BlockStack/BlockStack.aster"},
                {"command": "simulation.start"}, GET,
                input_keys("Left"), step(), GET,
                input_keys(), step(), input_keys("Right"), step(), GET,
                input_keys(), step(), input_keys("Up"), step(), GET,
                input_keys("Space"), step(), GET,
                input_keys("P"), step(), GET, input_keys(), step(120), GET,
                input_keys("P"), step(), GET,
                input_keys("R"), step(), GET,
                input_keys("Left"), step(120), GET,
                {"command": "simulation.stop"}]
    responses = run(editor, assets, requests)
    scenes = [response for request, response in zip(requests, responses) if request == GET]
    initial, left, right, rotated, dropped, paused, still_paused, resumed, reset, boundary = map(state, scenes)
    check(initial["status"] == "playing" and initial["piece"] == "O" and initial["x"] == 4, "game failed to spawn its opening piece")
    check(left["x"] == 3 and right["x"] == 4, "left/right input did not move the piece")
    check(rotated["rotation"] == 1, "rotation input did not update the piece")
    locked = [entity for entity in scenes[4]["Entities"] if entity["Name"].startswith("Block:")]
    check(len(locked) == 4 and dropped["score"] == 36, "hard drop did not lock four cells and score distance")
    check(paused["status"] == still_paused["status"] == "paused" and paused["y"] == still_paused["y"], "pause did not stop game time")
    check(resumed["status"] == "playing", "game did not resume")
    check(reset == initial and len(scenes[8]["Entities"]) == len(scenes[0]["Entities"]), "reset did not restore the board or leaked entities")
    check(boundary["x"] == 0 and boundary["y"] < 18, "held input or gravity failed")
    check(all(entity["Transform"]["Translation"][0] >= -4.5 for entity in scenes[9]["Entities"] if entity["Name"].startswith("Active:")),
          "piece escaped the left board boundary")

    # The next deterministic piece is asymmetric: verify actual occupied cells,
    # not just a rotation counter on the opening square.
    requests = [{"command": "scene.load", "path": "Games/BlockStack/BlockStack.aster"},
                {"command": "simulation.start"}, input_keys("Space"), step(), GET,
                input_keys("Up"), step(), GET, input_keys("Space"), step(), GET,
                {"command": "simulation.stop"}]
    responses = run(editor, assets, requests)
    horizontal, vertical, stacked = [response for request, response in zip(requests, responses) if request == GET]

    def active_positions(scene):
        return [entity["Transform"]["Translation"] for entity in scene["Entities"] if entity["Name"].startswith("Active:")]

    def extent(positions, axis):
        return max(position[axis] for position in positions) - min(position[axis] for position in positions)

    check(state(horizontal)["piece"] == state(vertical)["piece"] == "I", "deterministic bag fixture changed")
    check(extent(active_positions(horizontal), 0) == 3 and extent(active_positions(horizontal), 1) == 0,
          "I piece did not begin horizontal")
    check(extent(active_positions(vertical), 0) == 0 and extent(active_positions(vertical), 1) == 3,
          "rotation did not change the four rendered cell positions")
    locked = [entity for entity in stacked["Entities"] if entity["Name"].startswith("Block:")]
    check(len(locked) == 8 and len({tuple(entity["Transform"]["Translation"]) for entity in locked}) == 8,
          "hard drop overlapped an existing locked piece")
    i_rows = sorted(int(entity["Name"].split(":")[2]) for entity in locked if entity["Name"].endswith(":I"))
    check(i_rows == [2, 3, 4, 5], "vertical I did not stop exactly above the locked square")

    requests = [{"command": "scene.load", "path": "Games/BlockStack/BlockStack.aster"},
                {"command": "simulation.start"}, input_keys("Space"), step(), input_keys("Up"), step(),
                input_keys("Left"), step(60), GET, input_keys("Up"), step(), GET,
                {"command": "simulation.stop"}]
    responses = run(editor, assets, requests)
    wall_vertical, wall_rotated = [response for request, response in zip(requests, responses) if request == GET]
    check(state(wall_vertical)["x"] == 0 and state(wall_rotated)["x"] == 2,
          "rotation at the left wall did not apply a valid horizontal kick")
    check(extent(active_positions(wall_rotated), 0) == 3 and
          all(-4.5 <= position[0] <= 4.5 for position in active_positions(wall_rotated)),
          "wall rotation produced overlapping/out-of-bounds geometry")

    requests = [{"command": "scene.load", "path": "Games/BlockStack/BlockStack.aster"},
                {"command": "simulation.start"}, step(30), GET,
                {"command": "input.set", "input": {"Focused": False}}, step(120), GET,
                input_keys("R"), step(), input_keys("Down"), step(10), GET,
                {"command": "simulation.stop"}]
    responses = run(editor, assets, requests)
    focused, unfocused, soft = [state(response) for request, response in zip(requests, responses) if request == GET]
    check(focused["y"] == unfocused["y"] == 18, "unfocused window advanced the falling piece")
    check(soft["y"] == 14 and soft["score"] == 4, "soft drop did not accelerate movement and score each cell")

    requests = [{"command": "scene.load", "path": "Games/BlockStack/LineClear.aster"},
                {"command": "simulation.start"}, GET, input_keys("Space"), step(), GET,
                input_keys("R"), step(), GET, {"command": "simulation.stop"}]
    responses = run(editor, assets, requests)
    prepared, cleared, replay = [response for request, response in zip(requests, responses) if request == GET]
    check(sum(entity["Name"].startswith("Block:") for entity in prepared["Entities"]) == 16, "authored board seeds were not instantiated")
    victory = state(cleared)
    check(victory["status"] == "won" and victory["lines"] == 2 and victory["score"] == 336, "two-row clear/scoring/victory transition failed")
    check(not any(entity["Name"].startswith("Block:") for entity in cleared["Entities"]), "cleared rows retained cell entities")
    check(state(replay) == state(prepared) and len(replay["Entities"]) == len(prepared["Entities"]), "victory reset failed")

    requests = [{"command": "scene.load", "path": "Games/BlockStack/TopOut.aster"},
                {"command": "simulation.start"}, GET, input_keys("Space"), step(10), GET,
                input_keys("R"), step(), GET, {"command": "simulation.stop"}]
    responses = run(editor, assets, requests)
    over, stopped, replay = [state(response) for request, response in zip(requests, responses) if request == GET]
    check(over["status"] == stopped["status"] == replay["status"] == "lost", "spawn collision did not cause stable game over")
    check(over["score"] == stopped["score"] == replay["score"] == 0, "game-over state continued scoring")

    with tempfile.TemporaryDirectory(prefix="aster-blockstack-") as temporary:
        root = Path(temporary)
        source = root / "SourceAssets"
        shutil.copytree(assets, source)
        package = root / "Package"
        run(editor, source, [{"command": "project.export", "scene": "Games/BlockStack/BlockStack.aster",
                             "runtime": str(runtime), "notices": str(notices), "output": str(package)}])
        relocated = root / "Moved BlockStack"
        package.rename(relocated)
        shutil.rmtree(source)
        executable = package_executable(relocated)
        process = subprocess.run([str(executable), "--steps", "90"], cwd=root, capture_output=True,
                                 text=True, timeout=30)
        check(process.returncode == 0, f"Exported BlockStack failed: {process.stdout}\n{process.stderr}")
        result = json.loads(process.stdout)
        check(result["errors"] == [] and "BlockStack: ready" in result["log"], "exported game failed to initialize")
    print("BlockStack: movement, geometry rotation/wall kicks, gravity/soft-drop, collision/scoring, focus/pause/reset, row clear/victory, top-out and relocated export passed")


if __name__ == "__main__":
    main()
