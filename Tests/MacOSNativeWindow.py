"""Compile native macOS test drivers and verify existing, no-prompt permissions."""

import json
from pathlib import Path
import platform
import subprocess
import sys


def compile_helper(name, artifacts):
    if sys.platform != "darwin" or int(platform.mac_ver()[0].split(".")[0]) < 14:
        raise RuntimeError("Native macOS input requires macOS 14+ with an interactive desktop")
    assert name in ("MacOSRuntimeInput", "MacOSEditorInput"), "Unknown native helper"
    artifacts.mkdir(parents=True, exist_ok=True)
    helper_directory = artifacts / "NativeHelper"
    helper_directory.mkdir(exist_ok=True)
    helper = helper_directory / name
    architecture = platform.machine()
    assert architecture in ("arm64", "x86_64"), f"Unsupported native helper architecture: {architecture}"
    sources = [Path(__file__).with_name(filename).resolve()
               for filename in ("MacOSNativeWindow.swift", name + ".swift")]
    command = ["/usr/bin/xcrun", "swiftc", "-parse-as-library", "-swift-version", "5", "-O", "-warnings-as-errors",
               "-target", f"{architecture}-apple-macosx14.0", *map(str, sources), "-o", str(helper),
               "-module-cache-path", str(helper_directory / "ModuleCache")]
    for framework in ("AppKit", "ApplicationServices", "ScreenCaptureKit", "CryptoKit"):
        command.extend(("-framework", framework))
    compiled = subprocess.run(command, capture_output=True, text=True, timeout=120)
    (artifacts / f"{name}Build.log").write_text(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, f"Native macOS helper compilation failed: {compiled.stderr}"

    # Check this executable's actual permissions before launching its target app.
    # The interaction process repeats these checks before native operations.
    preflight = subprocess.run([str(helper), "--preflight"], capture_output=True, text=True, timeout=15)
    (artifacts / f"{name}Permissions.json").write_text(preflight.stdout)
    (artifacts / f"{name}Permissions.stderr").write_text(preflight.stderr)
    assert preflight.returncode == 0, \
        f"macOS native permissions are unavailable; no prompt or system change was attempted: {preflight.stderr}"
    permissions = json.loads(preflight.stdout)
    assert all(permissions.get(key) is True for key in ("accessibility", "post_event", "screen_capture")), \
        f"Native helper did not establish its required permissions: {permissions}"
    return helper, permissions
