"""Optional Linux Device-audio transport test; requires an existing PipeWire server.

Records only the launched editor's stereo stream, without changing endpoint volume,
defaults, or existing links. Endpoint metadata is evidence, not physical-listening proof.
"""

import argparse
import array
import json
import math
import os
from pathlib import Path
import selectors
import shutil
import signal
import subprocess
import sys
import time
import uuid


def properties(item):
    return item.get("info", {}).get("props", {})


def read_graph():
    return json.loads(subprocess.check_output(["pw-dump"], timeout=5))


def owns(graph, item, pid):
    clients = {client["id"] for client in graph if client["type"] == "PipeWire:Interface:Client"
               and str(properties(client).get("application.process.id")) == str(pid)}
    return (str(properties(item).get("application.process.id")) == str(pid)
            or properties(item).get("client.id") in clients)


def owned_node(graph, process, category):
    assert process.poll() is None, "Owned process exited before audio routing completed"
    matches = [item for item in graph if item["type"] == "PipeWire:Interface:Node"
               and owns(graph, item, process.pid) and properties(item).get("media.class") == category]
    assert len(matches) <= 1, f"Ambiguous PID-owned audio nodes: {[item['id'] for item in matches]}"
    return matches[0] if matches else None


def ports(graph, node, direction):
    matches = [item for item in graph if item["type"] == "PipeWire:Interface:Port"
               and properties(item).get("node.id") == node["id"]
               and properties(item).get("port.direction") == direction]
    if len(matches) < 2:
        return None
    result = {properties(item).get("audio.channel"): item for item in matches}
    assert len(matches) == 2 and set(result) == {"FL", "FR"}, "Owned stream requires exactly FL/FR ports"
    return result


def capture_links(graph, node):
    return [item["info"] for item in graph if item["type"] == "PipeWire:Interface:Link"
            and item.get("info", {}).get("input-node-id") == node["id"]]


def same_node(first, second):
    return (first is not None and second is not None and first["id"] == second["id"]
            and properties(first).get("object.serial") == properties(second).get("object.serial"))


def node_summary(item):
    keys = {"application.name", "application.process.id", "application.process.binary", "client.api",
            "client.id", "object.serial", "media.class", "media.name", "factory.name"}
    return {"id": item["id"], "state": item.get("info", {}).get("state"),
            "properties": {key: value for key, value in properties(item).items()
                           if key in keys or key.startswith(("node.", "device.", "api."))}}


def endpoint_evidence(graph, output, capture):
    targets = {item["info"]["input-node-id"] for item in graph
               if item["type"] == "PipeWire:Interface:Link"
               and item.get("info", {}).get("output-node-id") == output["id"]
               and item["info"]["input-node-id"] != capture["id"]}
    assert targets, "Aster has no existing OS playback route"
    result = []
    for endpoint in (item for item in graph if item["id"] in targets):
        record = node_summary(endpoint)
        device = next((item for item in graph if item["type"] == "PipeWire:Interface:Device"
                       and item["id"] == properties(endpoint).get("device.id")), None)
        record["device"] = node_summary(device) if device else None
        virtual = (properties(endpoint).get("node.virtual") in (True, "true")
                   or properties(endpoint).get("node.name") == "auto_null")
        record["endpoint_kind"] = "virtual" if virtual else "device-backed" if device else "unknown"
        result.append(record)
    return result


class EditorProtocol:
    def __init__(self, process):
        self.process = process
        self.pending = b""

    def request(self, command, **arguments):
        self.process.stdin.write(json.dumps({"command": command, **arguments}).encode() + b"\n")
        self.process.stdin.flush()
        deadline = time.monotonic() + 5
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while b"\n" not in self.pending:
                assert selector.select(max(0, deadline - time.monotonic())), f"Editor timed out: {command}"
                chunk = os.read(self.process.stdout.fileno(), 65536)
                assert chunk, f"Editor exited while executing {command}"
                self.pending += chunk
                assert len(self.pending) <= 4 * 1024 * 1024, "Editor response exceeded the fixture bound"
        line, self.pending = self.pending.split(b"\n", 1)
        response = json.loads(line)
        assert response.get("ok"), response
        result = response["result"]
        assert result.get("errors", []) == [], result
        return result


def stop_process(process):
    if process is not None:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        for stream in (process.stdin, process.stdout):
            if stream is not None:
                stream.close()


def analyze(wave_path):
    assert 0 < wave_path.stat().st_size <= 16 * 1024 * 1024, "Invalid capture file size"
    metadata = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_entries", "stream=codec_name,sample_rate,channels", "-of", "json",
        str(wave_path)], timeout=10))["streams"]
    assert len(metadata) == 1 and metadata[0] == {
        "codec_name": "pcm_f32le", "sample_rate": "48000", "channels": 2}, metadata
    pcm = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(wave_path),
                                   "-f", "f32le", "-acodec", "pcm_f32le", "-"], timeout=15)
    samples = array.array("f")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    assert all(math.isfinite(sample) for sample in samples), "Nonfinite captured PCM"
    duration = len(samples) / (48000 * 2)
    assert 11.8 <= duration <= 12.3, f"Incomplete or mistimed capture: {duration} seconds"
    levels = []
    for offset in range(0, len(samples) - 24000 + 1, 24000):
        block = samples[offset:offset + 24000]
        left, right = block[::2], block[1::2]
        rms = lambda channel: math.sqrt(sum(value * value for value in channel) / len(channel))
        crossings = sum(left[index - 1] <= 0 < left[index] for index in range(1, len(left)))
        levels.append({"seconds": offset / 96000, "left_rms": rms(left), "right_rms": rms(right),
                       "frequency": crossings * 4, "peak": max(abs(value) for value in block)})
    stages = []
    for index, name in enumerate(("LEFT", "CENTER", "RIGHT", "FAR", "SILENT", "LEFT")):
        # Leave half a second around each boundary for OS buffering/gain smoothing.
        stable = [value for value in levels if index * 2 + 0.5 <= value["seconds"] <= index * 2 + 1.5]
        assert len(stable) == 5, f"Missing stable samples for stage {name}"
        if name != "SILENT":
            assert all(abs(value["frequency"] - 440) <= 4 for value in stable), \
                f"Captured fixture tone is absent or corrupted in {name}: {stable}"
        stages.append({"stage": name, "left_rms": sum(value["left_rms"] for value in stable) / 5,
                       "right_rms": sum(value["right_rms"] for value in stable) / 5,
                       "peak": max(value["peak"] for value in stable)})
    left, center, right, far, silent, repeated = stages
    assert center["left_rms"] > 0.005 and center["right_rms"] > 0.005 and center["peak"] < 0.25, \
        f"Center stage is silent or clipped: {center}"
    assert left["left_rms"] > 1.5 * left["right_rms"] and right["right_rms"] > 1.5 * right["left_rms"], \
        "Captured spatial audio favors the wrong channels"
    assert abs(center["left_rms"] - center["right_rms"]) < 0.02 * center["left_rms"], "Center is unbalanced"
    assert abs(left["left_rms"] - right["right_rms"]) < 0.03 * left["left_rms"], "Left/right did not mirror"
    assert abs(left["right_rms"] - right["left_rms"]) < 0.03 * left["right_rms"], "Right/left did not mirror"
    assert silent["peak"] < 1e-6, "Silent stage contains audio"
    for channel in ("left_rms", "right_rms"):
        assert 0.02 * center[channel] < far[channel] < 0.25 * center[channel], "Distance attenuation failed"
        assert abs(repeated[channel] - left[channel]) < 0.03 * left[channel], "Audio failed to resume"
    return {"seconds": duration, "tone_hz": 440, "stages": stages, "levels": levels}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("editor", "assets", "artifacts"):
        parser.add_argument(f"--{argument}", type=Path, required=True)
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    evidence = {"passed": False, "scope": "PID-owned miniaudio Device output through the OS audio graph",
                "physical_listening_verified": False,
                "limitation": "An OS stream capture does not establish DAC/speaker output or human perception."}
    # Invalidate a previous successful report even if this run fails preflight.
    (artifacts / "PipeWireAudio.json").write_text(json.dumps(evidence, indent=2) + "\n")
    assert sys.platform.startswith("linux"), "PipeWire audio capture requires Linux"
    for tool in ("pw-record", "pw-dump", "pw-link", "ffmpeg", "ffprobe"):
        assert shutil.which(tool), f"Required audio test tool is missing: {tool}"
    editor_path, assets = (getattr(args, key).resolve() for key in ("editor", "assets"))
    assert editor_path.is_file() and (assets / "Scenes/AudioValidation.aster").is_file()
    node_name = "AsterOwnedAudioCapture-" + uuid.uuid4().hex
    wave_path = artifacts / "OwnedStream.wav"
    editor = recorder = None
    try:
        with (artifacts / "Editor.stderr").open("w") as editor_errors, \
             (artifacts / "Recorder.stderr").open("w") as recorder_errors:
            editor = subprocess.Popen([str(editor_path), "--automation", str(assets)],
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=editor_errors)
            protocol = EditorProtocol(editor)
            protocol.request("scene.load", path="Scenes/AudioValidation.aster")
            protocol.request("simulation.start", audio="device")
            recorder = subprocess.Popen([
                "pw-record", "--target=0", "--rate=48000", "--channels=2", "--channel-map=stereo", "--format=f32",
                "--properties={ node.name=" + node_name + " node.dont-reconnect=true stream.dont-remix=true }",
                str(wave_path)], stdout=subprocess.DEVNULL, stderr=recorder_errors)
            deadline = time.monotonic() + 5
            while True:
                graph = read_graph()
                output = owned_node(graph, editor, "Stream/Output/Audio")
                capture = owned_node(graph, recorder, "Stream/Input/Audio")
                if output and capture and ports(graph, output, "out") and ports(graph, capture, "in"):
                    break
                assert time.monotonic() < deadline, "PID-owned PipeWire nodes and stereo ports did not appear"
                time.sleep(0.02)
            assert properties(capture).get("node.name") == node_name
            assert not capture_links(graph, capture), "Recorder auto-connected unexpectedly"
            output_ports, input_ports = ports(graph, output, "out"), ports(graph, capture, "in")
            for channel in ("FL", "FR"):
                current = read_graph()
                assert same_node(owned_node(current, editor, "Stream/Output/Audio"), output), "Owned output changed"
                assert same_node(owned_node(current, recorder, "Stream/Input/Audio"), capture), "Owned recorder changed"
                current_output, current_input = ports(current, output, "out"), ports(current, capture, "in")
                assert current_output and current_output[channel]["id"] == output_ports[channel]["id"]
                assert current_input and current_input[channel]["id"] == input_ports[channel]["id"]
                subprocess.run(["pw-link", str(output_ports[channel]["id"]), str(input_ports[channel]["id"])],
                               check=True, timeout=5)
            graph = read_graph()
            links = capture_links(graph, capture)
            assert len(links) == 2 and all(link["output-node-id"] == output["id"] for link in links), \
                "Capture includes an unowned stream"
            evidence.update(editor_pid=editor.pid, recorder_pid=recorder.pid, source=node_summary(output),
                            capture=node_summary(capture), capture_links=links,
                            endpoints=endpoint_evidence(graph, output, capture), stage_timing=[])
            start = time.monotonic()
            for step in range(1, 721):
                time.sleep(max(0, start + step / 60 - time.monotonic()))
                assert recorder.poll() is None, "Recorder exited during playback"
                result = protocol.request("simulation.step", steps=1)
                elapsed = time.monotonic() - start
                assert elapsed < step / 60 + 0.25, "Host could not maintain real-time fixture stepping"
                if step % 120 == 0:
                    name = next(entity["Name"] for entity in result["scene"]["Entities"] if entity["ID"] == 2)
                    expected = ("CENTER", "RIGHT", "FAR", "SILENT", "LEFT", "CENTER")[step // 120 - 1]
                    assert name == "AudioValidation: " + expected, "Fixture stage sequence changed"
                    evidence["stage_timing"].append({"step": step, "seconds": elapsed, "stage": name})
            assert recorder.poll() is None
            recorder.send_signal(signal.SIGINT)
            evidence["recorder_exit"] = recorder.wait(timeout=5)
            # PipeWire 1.0.5 returns 1 on SIGINT even after closing the WAV normally;
            # only playback drain sets exit_code=0. Reject early exit and diagnostics.
            # https://github.com/PipeWire/pipewire/blob/1.0.5/src/tools/pw-cat.c#L1919-L1953
            assert evidence["recorder_exit"] in (0, 1) and not (artifacts / "Recorder.stderr").read_text(), \
                "Recorder failed; inspect Recorder.stderr"
            protocol.request("simulation.stop")
            editor.stdin.close()
            assert editor.wait(timeout=5) == 0, "Editor did not shut down normally"
        evidence.update(analyze(wave_path))
    finally:
        stop_process(recorder)
        stop_process(editor)
        (artifacts / "PipeWireAudio.json").write_text(json.dumps(evidence, indent=2) + "\n")
    deadline = time.monotonic() + 2
    while True:
        remaining = read_graph()
        leaked = any(properties(item).get("node.name") == node_name or
                     str(properties(item).get("application.process.id")) in
                     (str(evidence["editor_pid"]), str(evidence["recorder_pid"])) for item in remaining)
        if not leaked:
            break
        assert time.monotonic() < deadline, "Owned audio process left a client/stream alive"
        time.sleep(0.05)
    evidence["passed"] = True
    (artifacts / "PipeWireAudio.json").write_text(json.dumps(evidence, indent=2) + "\n")
    endpoints = [f"{endpoint['properties'].get('node.name', endpoint['id'])} ({endpoint['endpoint_kind']})"
                 for endpoint in evidence["endpoints"]]
    print(f"PipeWire owned-stream audio passed: {evidence['seconds']:.3f}s, 440 Hz, spatial stages, silence and repeat. "
          f"Endpoints: {', '.join(endpoints)}. Physical playback/listening remains unverified.")


if __name__ == "__main__":
    main()
