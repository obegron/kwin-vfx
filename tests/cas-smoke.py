#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Direct CAS, bypass and transitions to/from the optional AI backend."""
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
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        state = debug()
        assert not state["nvidia_error"], state
        if predicate(state):
            return state
        time.sleep(0.05)
    raise AssertionError(debug())


def difference(a, b, box):
    native = tuple(round(v * a.info["scale"]) for v in box)
    return max(v[1] for v in ImageChops.difference(a.crop(native), b.crop(native)).getextrema())


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
    configure(strength=1, cas_only=True, cas_strength=0)
    baseline = screenshot("cas-original")
    toggle()
    configure(strength=1, cas_only=True, cas_strength=1)
    filtered = screenshot("cas-only")
    state = debug()
    assert state["backend"] == "cas" and state["selected"] == 1, state
    assert state["nvidia_draws"] == 0 and not state.get("nvidia_jobs"), "CAS-only invoked the AI backend"
    target, control = (85, 105, 210, 170), (545, 105, 670, 170)
    assert difference(baseline, filtered, target) > 0
    assert difference(baseline, filtered, control) == 0
    assert difference(baseline, filtered, (335, 115, 395, 170)) <= 1
    configure(strength=1, cas_only=True, cas_strength=0)
    assert not debug()["processing"]
    assert difference(baseline, screenshot("cas-zero"), target) == 0
    configure(strength=0, cas_only=True, cas_strength=1, film_grain=1)
    assert not debug()["processing"] and not debug()["grain_animating"]
    assert difference(baseline, screenshot("cas-global-zero"), target) == 0
    ai_transition = False
    if state["nvidia_available"]:
        configure(strength=1, cas_only=False, cas_strength=0.5, nvidia_mode=12)
        wait(lambda d: d["backend"] == "nvidia-experimental" and d["nvidia_draws"] > 0)
        configure(strength=1, cas_only=True, cas_strength=1)
        wait(lambda d: not d.get("nvidia_jobs"))
        assert difference(filtered, screenshot("cas-after-ai"), target) == 0
        ai_transition = True
    toggle()
    assert debug()["selected"] == 0
    result = {"passed": True, "direct_cas_without_ai": True, "unselected_window_unchanged": True,
              "constant_color_preserved": True, "cas_zero_bypass": True, "global_zero_bypass": True,
              "ai_to_cas_transition": ai_transition, "hdr_output_seen": debug()["hdr_output_seen"],
              "native_size": baseline.size, "scale": baseline.info["scale"]}
    (artifacts / "cas-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
