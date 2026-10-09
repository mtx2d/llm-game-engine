"""Run renderer lifecycle checks with an owned Openbox instance on an empty X display."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def property_value(arguments, pattern):
    result = subprocess.run(["xprop", *arguments], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, f"Cannot query the test X display: {result.stderr}"
    match = re.search(pattern, result.stdout)
    return match.group(1) if match else None


def manager_window():
    return property_value(["-root", "_NET_SUPPORTING_WM_CHECK"], r"window id # (0x[0-9a-fA-F]+)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--window-manager", type=Path, required=True)
    args = parser.parse_args()
    assert os.environ.get("DISPLAY"), "A fresh X display is required; run this test through xvfb-run"
    assert manager_window() in (None, "0x0"), "Refusing to replace an existing window manager"

    with tempfile.TemporaryDirectory(prefix="AsterWindowManager-") as temporary:
        directory = Path(temporary)
        configuration = directory / "rc.xml"
        menu = directory / "menu.xml"
        menu.write_text('''<openbox_menu xmlns="http://openbox.org/3.4/menu">
  <menu id="root-menu" label="Aster validation" />
</openbox_menu>
''')
        configuration.write_text(f'''<?xml version="1.0"?>
<openbox_config xmlns="http://openbox.org/3.4/rc">
  <focus><focusNew>yes</focusNew></focus>
  <desktops><number>1</number></desktops>
  <theme><name>Clearlooks</name></theme>
  <menu><file>{menu}</file></menu>
</openbox_config>
''')
        environment = dict(os.environ, XDG_CONFIG_HOME=str(directory))
        with (directory / "Openbox.log").open("w+") as log:
            manager = subprocess.Popen([str(args.window_manager.resolve()), "--sm-disable", "--config-file",
                                        str(configuration)], env=environment, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 10
                while True:
                    assert manager.poll() is None, "Openbox exited before acquiring the test display"
                    window = manager_window()
                    if window and window != "0x0":
                        name = property_value(["-id", window, "_NET_WM_NAME"], r'= "([^"]+)"')
                        supporting = property_value(["-id", window, "_NET_SUPPORTING_WM_CHECK"],
                                                    r"window id # (0x[0-9a-fA-F]+)")
                        assert name == "Openbox" and supporting == window, "Unexpected window-manager identity"
                        # Openbox 3.6.1 does not publish a PID on its support
                        # window. Require it when available; the empty display,
                        # matching identity and live child establish this launch.
                        owner = property_value(["-id", window, "_NET_WM_PID"], r"= (\d+)")
                        assert owner in (None, str(manager.pid)), "Another process owns the window-manager selection"
                        break
                    assert time.monotonic() < deadline, "Openbox did not acquire the test display"
                    time.sleep(0.05)
                print(f"Window manager: Openbox (launched PID {manager.pid})", flush=True)
                result = subprocess.run([str(args.renderer.resolve())],
                                        env=dict(os.environ, ASTER_TEST_WINDOW="1", ASTER_TEST_WINDOW_MANAGER="1"),
                                        timeout=110)
                assert manager.poll() is None, "Window manager exited during renderer lifecycle checks"
                assert result.returncode == 0, f"Managed renderer test exited with {result.returncode}"
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), flush=True)
                raise
            finally:
                if manager.poll() is None:
                    manager.terminate()
                    try:
                        manager.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        manager.kill()
                        manager.wait(timeout=5)


if __name__ == "__main__":
    main()
