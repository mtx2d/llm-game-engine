# BlockStack

BlockStack is a playable 3D falling-block game authored using Aster's scene commands, Lua component/entity APIs, prefab spawning, input, audio, and runtime export. Clear two rows to win. If a new piece cannot enter the board, the game ends. The board, score, next piece, landing indicators, and status are rendered as 3D meshes.

Open `Games/BlockStack/BlockStack.aster` in the editor and press **Play**, or launch it with the graphical runtime. The main scene starts with an empty 10 × 20 board. The opening piece is a square; subsequent pieces come from a deterministic shuffled seven-piece bag. Rotation uses bounded horizontal/upward kicks, rather than claiming a particular commercial rotation specification.

| Control | Action |
| --- | --- |
| Left / A, Right / D | Move; hold to repeat |
| Up / X | Rotate clockwise |
| Z | Rotate counterclockwise |
| Down / S | Soft drop, one point per cell |
| Space | Hard drop, two points per cell |
| P | Pause or resume |
| R | Reset the current scenario |

Clearing one, two, three, or four rows awards 100, 300, 500, or 800 points. A short tone plays when rows clear. Leaving window focus suspends game movement until focus returns. Reset releases all runtime-created entities and restarts the same deterministic sequence.

`LineClear.aster` is a playable authored scenario with two almost-complete rows. Press Space to fill both gaps and win with the opening square. `TopOut.aster` exercises a board whose spawn area is already blocked. These use ordinary seed entities named `Seed:<column>:<row>`, with zero-based coordinates. The game script reads those authored seeds through the public entity API; it has no test-only control path.

Recreate the saved scenes and prefab through the same command protocol used by agents:

```sh
python3 Tests/CreateBlockStack.py --editor build/debug/AsterEditor --assets Assets
```

The generator creates entities, patches components, sets hierarchy/environment, and saves scenes through JSON commands. The game uses the repository's original cube mesh and audio fixture, and its licensed studio HDR environment. It adds no engine or editor scripting bindings.

Run the gameplay and export integration checks:

```sh
python3 Tests/BlockStackTests.py --editor build/debug/AsterEditor --runtime build/debug/AsterRuntime --assets Assets --notices ThirdParty
```

Tests inject public input snapshots and assert movement, rotation, repeated controls, gravity, locked board cells, drop scoring, pause, reset ownership, simultaneous row clearing, victory, and top-out behavior. They also export the actual game, move the package, delete its source asset copy, and launch the packaged runtime from another directory. Native graphical interaction and audio-device playback require their own platform validation in addition to these deterministic checks.
