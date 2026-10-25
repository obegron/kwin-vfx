#!/usr/bin/env python3
"""Real NVIDIA worker rendering, only in the isolated nested compositor."""
import json
import os
from pathlib import Path
import subprocess
import time
from PIL import ImageChops, ImageStat
from nested_support import artifacts, effects, script, screenshot, toggle, configure


def debug():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait_for(predicate, timeout=20):
    started = time.monotonic()
    while time.monotonic() - started < timeout:
        state = debug()
        if predicate(state):
            return state
        if state["nvidia_error"] and state["selected"] == 0:
            raise AssertionError(f"NVIDIA disabled itself: {state}")
        time.sleep(0.05)
    raise AssertionError(f"NVIDIA state timeout: {debug()}")


def crop(image, box):
    scale = image.info["scale"]
    return image.crop(tuple(round(v * scale) for v in box))


def difference(a, b, box):
    return max(pair[1] for pair in ImageChops.difference(crop(a, box), crop(b, box)).getextrema())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1"))
try:
    time.sleep(1)
    script("""
        let windows = workspace.windowList();
        let target = windows.find(w => w.caption === 'VFX test target');
        let control = windows.find(w => w.caption === 'VFX test control');
        target.frameGeometry = {x:60,y:80,width:360,height:240};
        control.frameGeometry = {x:520,y:80,width:360,height:240};
        workspace.activeWindow = target;
    """)
    configure(strength=1, denoise=0, sharpen=0)
    assert debug()["backend"] == "nvidia-experimental", debug()
    baseline = screenshot("nvidia-original")
    toggle()
    target = (85, 105, 210, 170)
    control = (545, 105, 670, 170)
    if os.environ.get("KWIN_VFX_TEST_NVIDIA_FAILURE") or os.environ.get("KWIN_VFX_EXPECT_NVIDIA_FAILURE"):
        state = wait_for(lambda d: d["selected"] == 0 and bool(d["nvidia_error"]))
        restored = screenshot("nvidia-failure-original")
        assert difference(baseline, restored, target) == 0
        assert difference(baseline, restored, control) == 0
        result = {"passed": True, "failure_original": True, "diagnostic": state["nvidia_error"]}
    else:
        state = wait_for(lambda d: d["nvidia_draws"] > 0)
        filtered = screenshot("nvidia-completed")
        before = ImageStat.Stat(crop(baseline, target)).var[0]
        after = ImageStat.Stat(crop(filtered, target)).var[0]
        assert after < before * 0.8, (before, after, debug())
        assert difference(baseline, filtered, control) == 0, "Unselected window changed"
        assert difference(baseline, filtered, (335, 115, 395, 170)) <= 3, "Orientation/constant color changed"
        mode_switches = []
        if os.environ.get("KWIN_VFX_TEST_MODES"):
            for mode in (12, 8):
                configure(strength=1, denoise=0, sharpen=0, nvidia_mode=mode)
                state = wait_for(lambda d: d["nvidia_mode"] == mode and d["nvidia_jobs"]
                    and all(j["state"] == 4 and j["mode"] == mode for j in d["nvidia_jobs"]))
                job = state["nvidia_jobs"][0]
                assert job["output_size"] == job["input_size"], job
                changed = screenshot(f"nvidia-mode-{mode}")
                assert difference(baseline, changed, control) == 0
                assert difference(baseline, changed, (335, 115, 395, 170)) <= 3
                assert difference(baseline, changed, target) > 0, "Mode did not change the fixture"
                if mode == 12:
                    configure(strength=1, denoise=0, sharpen=0, nvidia_mode=mode, nvidia_sharpen=1)
                    sharpened = screenshot("nvidia-deblur-sharpened")
                    assert debug()["nvidia_sharpen"] == 1
                    assert difference(changed, sharpened, target) > 0, "AI sharpening did not change the fixture"
                    assert difference(baseline, sharpened, control) == 0
                    # CAS can amplify quantization in an almost-flat SDK output;
                    # check brightness stability. Perfectly flat CAS input is
                    # tested separately in the independent GPU harness.
                    patch = (335, 115, 395, 170)
                    assert abs(ImageStat.Stat(crop(changed, patch)).mean[0]
                               - ImageStat.Stat(crop(sharpened, patch)).mean[0]) <= 1
                    configure(strength=1, denoise=0, sharpen=0, nvidia_mode=mode, nvidia_sharpen=0)
                    assert difference(changed, screenshot("nvidia-deblur-sharpen-off"), target) == 0
                configure(strength=0, denoise=0, sharpen=0, nvidia_mode=mode)
                assert difference(baseline, screenshot(f"nvidia-mode-{mode}-zero"), target) == 0
                mode_switches.append({"mode": mode, "input_size": job["input_size"], "output_size": job["output_size"]})
        configure(strength=0, denoise=0, sharpen=0)
        assert difference(baseline, screenshot("nvidia-zero"), target) == 0
        assert not debug()["processing"] and debug()["selected"] == 1
        configure(strength=1, denoise=0, sharpen=0)
        script("workspace.activeWindow.frameGeometry = {x:80,y:360,width:420,height:280};")
        old_draws = debug()["nvidia_draws"]
        state = wait_for(lambda d: d["nvidia_draws"] > old_draws)
        screenshot("nvidia-resized")
        assert state["selected"] == 1 and not state["nvidia_error"], state
        # A second independent window uses its own slot/worker.
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test control');")
        toggle()
        state = wait_for(lambda d: len(d["nvidia_jobs"]) == 2 and all(j["state"] == 4 for j in d["nvidia_jobs"]))
        both = screenshot("nvidia-two-windows")
        assert ImageStat.Stat(crop(both, control)).var[0] < before * 0.8
        toggle()
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test target');")
        toggle()
        assert debug()["selected"] == 0
        for _ in range(3):
            effects.unloadEffect("kwin_vfx")
            assert effects.loadEffect("kwin_vfx")
        result = {"passed": True, "variance_before": before, "variance_after": after,
                  "strength_zero": True, "resize": True, "two_windows": True, "reload_cycles": 3,
                  "mode_switches": mode_switches}
    result["native_size"] = baseline.size
    result["scale"] = baseline.info["scale"]
    (artifacts / "nvidia-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
