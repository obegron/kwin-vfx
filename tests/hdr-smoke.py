#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the actual KWin HDR shader and optional SDK pipeline privately.

Screenshots check SDR preview and transitions. They cannot certify physical HDR
output; output metadata is recorded separately, and HDR math has a float-FBO test.
"""
import json
import os
from pathlib import Path
import subprocess
import time
from PIL import ImageChops
from nested_support import artifacts, effects, script, screenshot, toggle, configure


def debug():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait(predicate):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        state = debug()
        assert not state["nvidia_error"], state
        if predicate(state):
            return state
        time.sleep(0.05)
    raise AssertionError(debug())


def difference(a, b, box):
    region = tuple(round(v * a.info["scale"]) for v in box)
    return max(pair[1] for pair in ImageChops.difference(a.crop(region), b.crop(region)).getextrema())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1"))
try:
    time.sleep(1)
    script("""
        const ws = workspace.windowList();
        const target = ws.find(w => w.caption === 'VFX test target');
        ws.find(w => w.caption === 'VFX test control').frameGeometry = {x:520,y:80,width:360,height:240};
        target.frameGeometry = {x:60,y:80,width:360,height:240};
        workspace.activeWindow = target;
    """)
    target, control = (85, 105, 210, 170), (545, 105, 670, 170)
    configure(strength=1, cas_only=True, cas_strength=0, true_hdr=True, hdr_peak=650)
    original = screenshot("hdr-original")
    toggle()
    state = wait(lambda d: d["nvidia_jobs"] and d["nvidia_jobs"][0]["state"] == 4 and d["nvidia_draws"] > 0)
    job = state["nvidia_jobs"][0]
    assert job["true_hdr"] and not job["restoration"], job
    converted = screenshot("hdr-only")
    assert difference(original, converted, target) > 0, "TrueHDR did not change the fixture"
    assert difference(original, converted, control) == 0, "Unselected window changed"
    configure(strength=0, cas_only=True, cas_strength=0, true_hdr=True, hdr_peak=650)
    assert difference(original, screenshot("hdr-zero"), target) == 0
    assert not debug()["processing"]
    configure(strength=1, cas_only=False, cas_strength=0.5, nvidia_mode=12, true_hdr=True, hdr_peak=650)
    state = wait(lambda d: d["nvidia_jobs"] and d["nvidia_jobs"][0]["restoration"] and d["nvidia_jobs"][0]["state"] == 4)
    combined = screenshot("hdr-deblur-cas")
    assert difference(converted, combined, target) > 0
    assert difference(original, combined, control) == 0
    configure(strength=1, cas_only=False, cas_strength=0.5, nvidia_mode=12, true_hdr=True, hdr_peak=650, film_grain=1)
    grain_a = screenshot("hdr-grain-a")
    time.sleep(0.15)
    grain_b = screenshot("hdr-grain-b")
    assert debug()["grain_animating"] and difference(grain_a, grain_b, target) > 0
    assert difference(original, grain_b, control) == 0
    configure(strength=1, cas_only=True, cas_strength=0, true_hdr=False, film_grain=0)
    wait(lambda d: not d.get("nvidia_jobs"))
    assert difference(original, screenshot("hdr-off"), target) == 0
    configure(strength=1, cas_only=True, cas_strength=0, true_hdr=True, hdr_peak=650)
    script("workspace.activeWindow.frameGeometry = {x:80,y:350,width:420,height:280};")
    wait(lambda d: d["nvidia_jobs"] and d["nvidia_jobs"][0]["state"] == 4 and d["nvidia_jobs"][0]["true_hdr"])
    screenshot("hdr-resized")
    toggle()
    assert debug()["selected"] == 0
    result = {"passed": True, "hdr_only": True, "deblur_cas_hdr": True,
              "whole_window_animated_grain": True, "zero_and_off_restore_original": True,
              "unselected_unchanged": True, "resize": True,
              "output_colors": state["output_colors"], "hdr_output_seen": state["hdr_output_seen"],
              "last_target_color": state.get("last_target_color"),
              "native_size": original.size, "scale": original.info["scale"]}
    (artifacts / "hdr-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
