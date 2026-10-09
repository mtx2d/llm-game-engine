# Aster audio verification on macOS

Run the native audio callback check, then listen to the spatial audio scene through stereo headphones or speakers. The listening result establishes physical output on your tested Mac and output device; Windows and Ubuntu checks remain separate.

## Prepare the Mac

Select your headphones or speakers as the Mac's sound output. Use a comfortable volume and keep the output device connected throughout the checks.

The build requires macOS 13 or newer on a Metal-capable Mac, Xcode Command Line Tools, CMake 3.24 or newer, Ninja, and Python 3. The bootstrap script checks its command-line prerequisites and reports anything missing. If a command fails, stop and send the error output.

Use your existing Aster checkout, or clone it:

```bash
git clone https://github.com/mtx2d/llm-game-engine.git
cd llm-game-engine
```

Run the remaining commands from the repository root. Record the revision and Mac details for your report:

```bash
git rev-parse HEAD
sw_vers
uname -m
```

## Build the native editor and runtime

Run these commands in the same Terminal or tmux shell. The first build downloads and builds pinned dependencies under `build/`.

```bash
bash scripts/BootstrapMacOS.sh
source build/macos-vulkan/Environment.sh

cmake -S . -B build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos --target AsterEditor AsterRuntime --parallel 3
```

Skip rebuilding only if your existing native binaries are up to date with the Git revision you recorded. In a new shell, run `source build/macos-vulkan/Environment.sh` before launching the executables.

## Check the native audio callback

```bash
python3 Tests/DeviceAudioTests.py \
  --editor build/macos/AsterEditor \
  --artifacts build/macos/device-audio-evidence
```

Expect a message beginning `Native device audio passed`. This checks two natural playback completions, restart, explicit stop/replay, and normal shutdown. The result is saved to `build/macos/device-audio-evidence/DeviceAudio.json`; diagnostics are in the same directory's `Editor.stderr`.

The report intentionally leaves `physical_output_verified` false. The next check supplies your listening observations; callback progress alone cannot establish what the speakers or headphones produced.

## Listen to the spatial sequence

Run the command exactly as shown. Adding `--steps` selects offline audio.

```bash
build/macos/AsterRuntime \
  --scene Assets/Scenes/AudioValidation.aster \
  --project Assets
```

Listen for at least two complete cycles. Each stage lasts approximately two seconds:

1. **Left:** tone mostly in the left channel.
2. **Center:** tone balanced between both channels.
3. **Right:** tone mostly in the right channel.
4. **Far:** centered tone, noticeably quieter than the center stage.
5. **Silent:** no tone.

The ten-second sequence repeats. Close the game window and confirm the tone stops. After closing, the terminal prints a JSON result containing the stage log and an `errors` list, which should be empty. Report persistent distortion, unexpected interruptions, reversed channels, or any other mismatch.

## Send the results

Reply with the following filled in, and include `build/macos/device-audio-evidence/DeviceAudio.json` plus any terminal errors:

```text
Git revision:
Mac model and CPU:
macOS version:
Output device and connection (built-in, wired, USB, or Bluetooth):
Native callback check (passed or error):
Left, center, right placement correct:
Far stage noticeably quieter:
Silent stage silent:
Sequence repeated correctly for at least two cycles:
Sound stopped when the window closed:
Runtime errors list empty:
Other observations:
```

To read these instructions inside tmux, run `less docs/AudioVerification.md`. Use the arrow keys or Page Up/Page Down to scroll, and press `q` to exit.
