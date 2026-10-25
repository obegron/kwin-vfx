#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Animated GPU grain, exact bypass, unaffected windows and cached AI reuse."""
import json
import os
from pathlib import Path
import subprocess
import time

import numpy as np
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


def crop(image, box):
    return image.crop(tuple(round(v * image.info["scale"]) for v in box))


def difference(a, b, box):
    return max(pair[1] for pair in ImageChops.difference(crop(a, box), crop(b, box)).getextrema())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1"))
try:
    time.sleep(1)
    script("""
        const windows = workspace.windowList();
        const target = windows.find(w => w.caption === 'VFX test target');
        const control = windows.find(w => w.caption === 'VFX test control');
        target.frameGeometry = {x:60,y:80,width:360,height:240};
        control.frameGeometry = {x:520,y:80,width:360,height:240};
        workspace.activeWindow = target;
    """)
    configure(strength=1, denoise=0, sharpen=0, film_grain=0)
    original = screenshot("grain-original")
    toggle()
    if debug()["backend"] == "nvidia-experimental":
        wait(lambda d: d["nvidia_draws"] > 0 and d["nvidia_jobs"] and all(j["state"] == 4 for j in d["nvidia_jobs"]))
    baseline = screenshot("grain-filtered-baseline")
    before = debug()
    target = (85, 105, 210, 170)
    flat = (335, 115, 395, 170)
    control = (545, 105, 670, 170)
    configure(strength=1, denoise=0, sharpen=0, film_grain=1)
    wait(lambda d: d["grain_animating"] and d["grain_frame"] != before["grain_frame"])
    first = screenshot("grain-first")
    time.sleep(0.18)
    second = screenshot("grain-second")
    after = debug()
    assert difference(first, second, flat) > 0, "Grain did not animate on a static frame"
    assert difference(baseline, first, control) == 0
    assert difference(baseline, second, control) == 0
    delta = np.asarray(crop(first, flat).convert("RGB"), dtype=np.int16) - np.asarray(crop(baseline, flat).convert("RGB"), dtype=np.int16)
    assert np.abs(delta).max() <= 22, "Grain exceeded its amplitude bound"
    assert np.abs(delta.mean()) < 1, "Grain noticeably changed average brightness"
    assert np.abs(delta[:, :, 0] - delta[:, :, 1]).max() <= 1, "Grain introduced chroma noise"
    assert np.abs(delta[:, :, 1] - delta[:, :, 2]).max() <= 1
    assert delta[:, :, 0].var() > 2, "Grain is missing from the flat patch"
    if before["backend"] == "nvidia-experimental":
        assert [j["submitted_revision"] for j in before["nvidia_jobs"]] == [j["submitted_revision"] for j in after["nvidia_jobs"]], "Grain resubmitted an unchanged frame to AI"
    configure(strength=1, denoise=0, sharpen=0, film_grain=0)
    assert not debug()["grain_animating"]
    assert difference(baseline, screenshot("grain-off-restored"), target) == 0
    assert difference(baseline, screenshot("grain-flat-restored"), flat) == 0
    configure(strength=0, denoise=0, sharpen=0, film_grain=1)
    assert not debug()["grain_animating"]
    assert difference(original, screenshot("grain-global-bypass"), target) == 0
    configure(strength=1, denoise=0, sharpen=0, film_grain=1)
    assert debug()["grain_animating"]
    toggle()
    assert not debug()["grain_animating"] and debug()["selected"] == 0
    result = {"passed": True, "backend": before["backend"], "animated_on_static_frame": True,
              "grain_delta_variance": float(delta[:, :, 0].var()), "max_rgb_delta": int(np.abs(delta).max()),
              "mean_rgb_delta": float(delta.mean()), "monochrome": True, "off_restores_exactly": True,
              "global_zero_bypass": True, "independent_window_unchanged": True,
              "unchanged_ai_input_not_resubmitted": before["backend"] == "nvidia-experimental",
              "timer_stops_when_disabled_or_unselected": True, "native_size": original.size,
              "scale": original.info["scale"]}
    (artifacts / "grain-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
