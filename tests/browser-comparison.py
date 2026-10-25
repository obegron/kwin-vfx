#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare a paused Firefox video through the real nested KWin effect."""
import argparse
import json
import os
from pathlib import Path
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--preview-session", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
session = json.loads(args.preview_session.read_text())
run_dir = Path(session["KWIN_VFX_RUN_DIR"])
assert run_dir.resolve() == args.preview_session.resolve().parent
assert run_dir.name.startswith("kwin-vfx-nested.")
assert session["WAYLAND_DISPLAY"].startswith("kwin-vfx-kwin-vfx-nested.")
assert session["DBUS_SESSION_BUS_ADDRESS"] != os.environ.get("DBUS_SESSION_BUS_ADDRESS")
os.environ.update(session)
os.environ["KWIN_VFX_NESTED"] = "1"
os.environ["XDG_CONFIG_HOME"] = str(run_dir / "config")
from nested_support import bus, effects, screenshot, configure
import dbus
from PIL import ImageChops, ImageStat

destination = args.output_dir.resolve()
destination.mkdir(parents=True, exist_ok=True)


def debug():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait(mode):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        state = debug()
        assert not state["nvidia_error"], state
        if state["nvidia_mode"] == mode and state["nvidia_jobs"] and all(
                j["state"] == 4 and j["mode"] == mode for j in state["nvidia_jobs"]):
            time.sleep(0.15)
            return state
        time.sleep(0.05)
    raise AssertionError(debug())


saved = debug()
assert saved["nvidia_available"] and saved["selected"] == 1, saved
players = [str(name) for name in bus.list_names() if str(name).startswith("org.mpris.MediaPlayer2.firefox")]
assert len(players) == 1, "Expected one Firefox MPRIS player; muted media may not register one"
obj = bus.get_object(players[0], "/org/mpris/MediaPlayer2")
player = dbus.Interface(obj, "org.mpris.MediaPlayer2.Player")
props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
was_playing = str(props.Get("org.mpris.MediaPlayer2.Player", "PlaybackStatus")) == "Playing"
frames = {}
try:
    player.Pause()
    time.sleep(4)
    position = int(props.Get("org.mpris.MediaPlayer2.Player", "Position"))
    configure(strength=0, nvidia_mode=12, nvidia_sharpen=0, film_grain=0)
    frames["off"] = screenshot("compare-off")
    states = {}
    for name, mode, grain in (("denoise", 8, 0), ("deblur", 12, 0), ("deblur-grain", 12, 0.5)):
        configure(strength=1, nvidia_mode=mode, nvidia_sharpen=0, film_grain=grain)
        states[name] = wait(mode)
        frames[name] = screenshot("compare-" + name)
        assert str(props.Get("org.mpris.MediaPlayer2.Player", "PlaybackStatus")) == "Paused"
        assert int(props.Get("org.mpris.MediaPlayer2.Player", "Position")) == position
    configure(strength=0, nvidia_mode=12, nvidia_sharpen=0, film_grain=0)
    frames["off-restored"] = screenshot("compare-off-restored")
    differences = {}
    for name, frame in frames.items():
        frame.save(destination / (name + ".png"))
        delta = ImageChops.difference(frames["off"].convert("RGB"), frame.convert("RGB"))
        differences[name] = {"max_rgb_delta": max(v[1] for v in delta.getextrema()),
                             "mean_abs_rgb_delta": sum(ImageStat.Stat(delta).mean) / 3}
    metadata = props.Get("org.mpris.MediaPlayer2.Player", "Metadata")
    report = {"passed": True, "same_paused_mpris_position": position,
              "title": str(metadata.get("xesam:title", "")), "url": str(metadata.get("xesam:url", "")),
              "size": frames["off"].size, "scale": frames["off"].info["scale"],
              "differences_from_off": differences,
              "modes": {name: {"mode": state["nvidia_mode"], "grain": state["film_grain"], "jobs": state["nvidia_jobs"]} for name, state in states.items()},
              "limits": "Same paused browser video; browser UI may animate. No high-resolution reference or verified decoded resolution. Pixel differences are not a quality score. Grain is one sample of animated noise. All comparisons use 100% blend and additional sharpening Off."}
    (destination / "comparison.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "output": str(destination), "differences_from_off": differences}), flush=True)
finally:
    configure(strength=saved["strength"], denoise=saved["denoise"], sharpen=saved["sharpen"],
              nvidia_mode=saved["nvidia_mode"], nvidia_sharpen=saved["nvidia_sharpen"], film_grain=saved["film_grain"],
              cas_only=saved["cas_only"], cas_strength=saved["cas_strength"])
    if was_playing:
        player.Play()
