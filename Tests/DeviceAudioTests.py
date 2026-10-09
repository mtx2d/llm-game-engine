"""Optional native device-clock audio test using only owned temporary assets.

No PCM is pulled by this test. A finite source must reach EOF while the editor
receives no commands, proving that the OS device callback advances its graph.
This does not capture audio or establish physical speaker/headphone output.
"""

import argparse
import array
import json
import math
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import wave


# Exact descriptions from the pinned miniaudio dependency. Other initialization
# errors (including allocation, argument and start failures) remain test failures.
UNAVAILABLE_ERRORS = {
    "miniaudio device initialization failed: " + description
    for description in ("No backend", "No device", "Failed to initialize backend", "Failed to open backend device")
}


class EditorProtocol:
    def __init__(self, process, transcript):
        self.process, self.transcript = process, transcript
        self.responses = queue.Queue()
        self.sequence = 0
        self.reader = threading.Thread(target=self.read_responses, daemon=True)
        self.reader.start()

    def read_responses(self):
        try:
            while True:
                line = self.process.stdout.readline(4 * 1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 4 * 1024 * 1024 or not line.endswith(b"\n"):
                    raise RuntimeError("Editor response exceeded its bound or ended without a newline")
                self.responses.put(json.loads(line))
        except Exception as error:
            self.responses.put(error)
        finally:
            self.responses.put(None)

    def request(self, command, **arguments):
        self.sequence += 1
        request = {"id": self.sequence, "command": command, **arguments}
        self.process.stdin.write(json.dumps(request).encode() + b"\n")
        self.process.stdin.flush()
        try:
            response = self.responses.get(timeout=10)
        except queue.Empty as error:
            raise AssertionError(f"Editor timed out while executing {command}") from error
        if isinstance(response, Exception):
            raise response
        assert isinstance(response, dict), f"Editor exited while executing {command}"
        assert response.get("id") == self.sequence, "Editor response did not match its request"
        self.transcript.append({"request": request, "response": response})
        return response


def result(response):
    assert response.get("ok"), response
    value = response["result"]
    assert value.get("errors", []) == [], value
    return value


def write_fixture(project, probe):
    scene = {"Version": 1, "Name": "Native device clock validation", "NextEntityID": 2, "Entities": []}
    if not probe:
        sample_rate = 48000
        samples = array.array("h")
        for frame in range(sample_rate):
            envelope = min(frame / 480, (sample_rate - 1 - frame) / 480, 1)
            sample = round(0.02 * 32767 * envelope * math.sin(2 * math.pi * 440 * frame / sample_rate))
            samples.extend((sample, sample))
        if sys.byteorder != "little":
            samples.byteswap()
        with wave.open(str(project / "QuietTone.wav"), "wb") as output:
            output.setnchannels(2)
            output.setsampwidth(2)
            output.setframerate(sample_rate)
            output.writeframes(samples.tobytes())
        (project / "DeviceClock.lua").write_text('''return {
    OnCreate = function(self, entity)
        self.phase = 0
        assert(not engine.is_audio_playing(entity), "Source started before the explicit play")
        engine.play_audio(entity)
        assert(engine.is_audio_playing(entity), "Initial device source did not start")
        engine.set_name(entity, "DeviceAudio: initial playing")
    end,
    OnUpdate = function(self, entity, dt)
        assert(not engine.is_audio_playing(entity), "Device clock did not consume the finite source to EOF")
        if self.phase == 0 then
            engine.play_audio(entity)
            assert(engine.is_audio_playing(entity), "Restart after EOF failed")
            engine.stop_audio(entity)
            assert(not engine.is_audio_playing(entity), "Explicit stop left the device source playing")
            engine.play_audio(entity)
            assert(engine.is_audio_playing(entity), "Replay after stop failed")
            engine.set_name(entity, "DeviceAudio: restarted playing")
            self.phase = 1
        elseif self.phase == 1 then
            engine.set_name(entity, "DeviceAudio: completed")
            self.phase = 2
        else
            error("Unexpected extra simulation step in device-clock fixture")
        end
    end
}
''')
        scene["Entities"] = [{"ID": 1, "Name": "Device audio source", "Parent": None,
            "Transform": {"Translation": [0, 0, 0], "Rotation": [0, 0, 0], "Scale": [1, 1, 1]},
            "AudioSource": {"Path": "QuietTone.wav", "Volume": 1, "Pitch": 1,
                            "Loop": False, "PlayOnStart": False, "Spatial": False},
            "Script": {"Path": "DeviceClock.lua", "Enabled": True}}]
    (project / "DeviceClock.aster").write_text(json.dumps(scene))


def stop_process(process, protocol):
    if process is not None:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        if process.stdin:
            process.stdin.close()
        if protocol:
            protocol.reader.join(timeout=2)
            assert not protocol.reader.is_alive(), "Editor response reader did not exit after process shutdown"
        if process.stdout:
            process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--editor", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--probe", action="store_true",
                        help="Only probe native device initialization; unavailable is recorded without failing")
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    evidence = {"passed": False, "status": "running", "mode": "probe" if args.probe else "test",
                "platform": sys.platform, "device_available": None, "device_clock_eof_verified": False,
                "physical_output_verified": False, "pcm_capture_verified": False,
                "scope": "Native device initialization only" if args.probe else "OS device-clock source consumption",
                "responses": [], "waits": []}
    report = artifacts / "DeviceAudio.json"
    report.write_text(json.dumps(evidence, indent=2) + "\n")
    process = protocol = None
    try:
        editor = args.editor.resolve()
        assert editor.is_file(), f"Editor executable is missing: {editor}"
        with tempfile.TemporaryDirectory(prefix="AsterDeviceAudio-") as temporary, \
             (artifacts / "Editor.stderr").open("w") as diagnostics:
            project = Path(temporary)
            write_fixture(project, args.probe)
            process = subprocess.Popen([str(editor), "--automation", str(project)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=diagnostics)
            protocol = EditorProtocol(process, evidence["responses"])
            result(protocol.request("scene.load", path="DeviceClock.aster"))
            baseline = result(protocol.request("scene.get"))
            started = protocol.request("simulation.start", audio="device")
            if not started.get("ok") and started.get("error") in UNAVAILABLE_ERRORS:
                evidence.update(status="unavailable", device_available=False, error=started["error"])
            else:
                assert result(started)["playing"] is True
                evidence["device_available"] = True
                if not args.probe:
                    initial = result(protocol.request("scene.get"))
                    assert initial["Entities"][0]["Name"] == "DeviceAudio: initial playing"
                    for stage in ("restarted playing", "completed"):
                        # No commands, simulation steps or PCM reads occur here.
                        # Only the independently running native device can consume audio.
                        start = time.monotonic()
                        time.sleep(2.0)
                        evidence["waits"].append(time.monotonic() - start)
                        stepped = result(protocol.request("simulation.step", steps=1))
                        assert stepped["scene"]["Entities"][0]["Name"] == "DeviceAudio: " + stage
                    evidence["device_clock_eof_verified"] = True
                assert result(protocol.request("simulation.stop"))["playing"] is False
                assert result(protocol.request("scene.get")) == baseline, "Stop did not restore authored source state"
                evidence["status"] = "available" if args.probe else "passed"
            process.stdin.close()
            assert process.wait(timeout=10) == 0, "Editor failed to shut down normally"
    except BaseException as error:
        evidence.update(status="failed", passed=False, error=str(error))
        raise
    finally:
        try:
            stop_process(process, protocol)
        except BaseException as error:
            evidence.update(status="failed", passed=False, error=str(error))
            raise
        finally:
            evidence["passed"] = evidence["status"] == "passed"
            report.write_text(json.dumps(evidence, indent=2) + "\n")
    if evidence["status"] == "unavailable":
        print(f"Native audio device unavailable: {evidence['error']}. Device playback remains unverified.")
        return 0 if args.probe else 1
    if args.probe:
        print("Native audio device initialized. Probe only: device-clock playback and physical output remain unverified.")
    else:
        print("Native device audio passed: two natural EOF transitions, restart, stop/replay and normal shutdown. "
              "PCM capture and physical output remain unverified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
