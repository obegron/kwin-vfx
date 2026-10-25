#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stable-ID selection without changing focus; run only on the private bus."""
import configparser
import json
import os
from pathlib import Path
import subprocess
import time
import uuid
import dbus
from PIL import ImageChops
from nested_support import artifacts, bus, effects, script, screenshot, configure


def control():
    return dbus.Interface(bus.get_object("org.kde.KWin", "/WindowEnhancement"), "org.kde.KWin.WindowEnhancement")


def windows():
    return json.loads(control().windowList())


def state():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait(predicate):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        current = state()
        assert not current["nvidia_error"], current
        if predicate(current):
            return current
        time.sleep(.05)
    raise AssertionError(state())


def difference(a, b, box):
    box = tuple(round(v * a.info["scale"]) for v in box)
    return max(pair[1] for pair in ImageChops.difference(a.crop(box), b.crop(box)).getextrema())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1"))
try:
    time.sleep(1)
    script("""
        const ws=workspace.windowList();
        const target=ws.find(w=>w.caption==='VFX test target');
        const other=ws.find(w=>w.caption==='VFX test control');
        target.frameGeometry={x:60,y:80,width:360,height:240};
        other.frameGeometry={x:520,y:80,width:360,height:240};
        workspace.activeWindow=other;
    """)
    configure(strength=1, cas_only=True, cas_strength=1)
    listed = windows()
    target = next(w for w in listed if w["title"] == "VFX test target")
    other = next(w for w in listed if w["title"] == "VFX test control")
    assert target["id"] != other["id"] and target["application"] == other["application"]
    assert other["active"] and not target["active"]
    original = screenshot("window-control-original")
    assert control().setWindowEnabled(target["id"], True)
    assert control().setWindowEnabled(target["id"], True), "Selection must be idempotent"
    selected = windows()
    assert next(w for w in selected if w["id"] == target["id"])["enabled"]
    assert next(w for w in selected if w["id"] == other["id"])["active"], "Selecting stole focus"
    assert state()["selected"] == 1
    filtered = screenshot("window-control-one")
    assert difference(original, filtered, (85,105,210,170)) > 0
    assert difference(original, filtered, (545,105,670,170)) == 0
    assert not control().setWindowEnabled("not-a-window", True)
    assert not control().setWindowEnabled(str(uuid.uuid4()), True)
    assert state()["selected"] == 1
    assert control().setWindowEnabled(other["id"], True)
    assert state()["selected"] == 2
    control().disableAllWindows()
    assert state()["selected"] == 0 and not any(w["enabled"] for w in windows())
    assert difference(original, screenshot("window-control-all-off"), (85,105,210,170)) == 0
    assert control().setWindowEnabled(target["id"], True)
    script("workspace.windowList().find(w=>w.caption==='VFX test target').closeWindow();")
    wait(lambda d: d["selected"] == 0)
    assert not any(w["id"] == target["id"] for w in windows())
    assert not control().setWindowEnabled(target["id"], True)
    # Exercise the production opt-in through a PRIVATE compositor. No env runtime
    # is supplied by run-nested.sh, so this reconstructs the config-based branch.
    effects.unloadEffect("kwin_vfx")
    path = Path(os.environ["XDG_CONFIG_HOME"]) / "kwinrc"
    config = configparser.ConfigParser(); config.optionxform = str; config.read(path)
    config["Effect-kwin_vfx"].update({"NvidiaEnabled":"true", "NvidiaRuntime":str(Path("build-nvidia/runtime").resolve()),
        "CasOnly":"false", "NvidiaMode":"12", "TrueHdr":"true", "HdrPeakNits":"650", "FilmGrain":"0"})
    with path.open("w") as file: config.write(file, space_around_delimiters=False)
    assert effects.loadEffect("kwin_vfx")
    assert state()["nvidia_available"] and state()["control_available"]
    assert control().setWindowEnabled(other["id"], True)
    rendered = wait(lambda d: d["nvidia_draws"] > 0 and bool(d["nvidia_jobs"]))
    hdr_available = rendered["hdr_available"]
    assert rendered["true_hdr"] == hdr_available and rendered["nvidia_jobs"][0]["true_hdr"] == hdr_available
    assert rendered["true_hdr_requested"], "HDR preference should be remembered on an SDR display"
    control().disableAllWindows()
    wait(lambda d: d["retiring_ai_slots"] == 0)
    for _ in range(3):
        effects.unloadEffect("kwin_vfx")
        assert effects.loadEffect("kwin_vfx")
        assert state()["control_available"] and windows()
    result = {"passed":True, "stable_window_ids":True, "same_application_independent":True,
        "selection_preserves_focus":True, "idempotent_selection":True, "invalid_and_closed_ids_rejected":True,
        "unselected_pixels_unchanged":True, "disable_all":True, "desktop_config_opt_in":True,
        "deblur_from_config":True, "hdr_available":hdr_available, "hdr_safely_gated":True, "reload_cycles":3}
    (artifacts / "window-control-result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)
finally:
    client.terminate(); client.wait(timeout=10)
