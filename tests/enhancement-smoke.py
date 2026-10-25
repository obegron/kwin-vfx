#!/usr/bin/env python3
"""Shader filter correctness/lifecycle at native fractional scale in nested KWin."""
import json
import os
from pathlib import Path
import subprocess
import time

from PIL import ImageChops, ImageStat
from nested_support import artifacts, bus, effects, script, screenshot, pixel, same, toggle, accel, configure


def settings(strength=1, denoise=1, sharpen=0):
    configure(strength, denoise, sharpen)


def debug(parameter=""):
    return json.loads(effects.debug("kwin_vfx", parameter, signature="ss"))


def crop(image, box):
    scale = image.info["scale"]
    return image.crop(tuple(round(v * scale) for v in box))


def same_crop(a, b, box, tolerance=0):
    extrema = ImageChops.difference(crop(a, box), crop(b, box)).getextrema()
    return max(extreme[1] for extreme in extrema) <= tolerance


env = dict(os.environ, KWIN_VFX_TEST_PATTERN="1")
client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")], env=env)
try:
    time.sleep(1)
    script("""
        let windows = workspace.windowList();
        let target = windows.find(w => w.caption === 'VFX test target');
        let control = windows.find(w => w.caption === 'VFX test control');
        target.frameGeometry = {x: 60, y: 80, width: 360, height: 240};
        control.frameGeometry = {x: 520, y: 80, width: 360, height: 240};
        workspace.activeWindow = target;
    """)
    settings()
    assert not debug()["test_tint"]
    time.sleep(0.3)
    original = screenshot("filter-disabled")
    toggle()
    filtered = screenshot("denoise-enabled")
    target_box = (85, 105, 210, 170)
    control_box = (545, 105, 670, 170)
    variance_before = ImageStat.Stat(crop(original, target_box)).var[0]
    variance_after = ImageStat.Stat(crop(filtered, target_box)).var[0]
    failure = os.environ.get("KWIN_VFX_TEST_FAILURE", "")
    if failure:
        assert same_crop(original, filtered, target_box), "Failure must draw original"
        assert debug()["selected"] == 0
        assert same_crop(original, filtered, control_box)
        result = {"passed": True, "failure_mode": failure}
    else:
        assert variance_after < variance_before * 0.8, (variance_before, variance_after)
        assert same_crop(original, filtered, control_box), "Control changed"
        assert same(pixel(original, 350, 130), pixel(filtered, 350, 130)), "Solid fill changed"
        # Zero strength retains selection but removes processing and scanout restriction.
        settings(strength=0)
        zero = screenshot("strength-zero")
        assert same_crop(original, zero, target_box), "Zero strength must bypass capture/filter"
        assert debug()["selected"] == 1 and not debug()["processing"]
        accel.invokeShortcut("IncreaseWindowEnhancementStrength", "default", signature="ss")
        time.sleep(0.1)
        assert abs(debug()["strength"] - 0.1) < 0.001
        accel.invokeShortcut("DecreaseWindowEnhancementStrength", "default", signature="ss")
        time.sleep(0.1)
        assert debug()["strength"] == 0 and not debug()["processing"]
        settings(strength=0.3)
        low = screenshot("strength-low")
        settings(strength=1)
        high = screenshot("strength-high")
        assert ImageStat.Stat(crop(high, target_box)).var[0] < ImageStat.Stat(crop(low, target_box)).var[0]
        # Independent selection persists through strength changes and movement.
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test control');")
        toggle()
        both = screenshot("both-filtered")
        assert ImageStat.Stat(crop(both, control_box)).var[0] < variance_before * 0.8
        toggle()
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test target');")
        for _ in range(100): toggle()
        script("workspace.activeWindow.frameGeometry = {x:80, y:360, width:420, height:280};")
        time.sleep(0.2)
        moved = screenshot("filter-moved-resized")
        assert debug()["selected"] == 1
        assert same_crop(original, moved, control_box)
        script("workspace.activeWindow.frameGeometry = {x:60, y:80, width:360, height:240};")
        time.sleep(0.2)
        # Exercise pure sharpening through the actual KWin shader as well.
        settings(denoise=0, sharpen=1)
        sharpened = screenshot("sharpen-enabled")
        assert not same_crop(original, sharpened, target_box)
        toggle()
        assert same_crop(original, screenshot("filter-restored"), target_box)
        toggle()
        script("workspace.activeWindow.closeWindow();")
        time.sleep(0.2)
        assert debug()["selected"] == 0
        for _ in range(20):
            effects.unloadEffect("kwin_vfx")
            assert effects.loadEffect("kwin_vfx")
        result = {"passed": True, "noise_variance_before": variance_before, "noise_variance_after": variance_after,
            "zero_bypass": True, "strength_shortcuts": True, "independent_windows": True,
            "move_resize": True, "toggle_cycles": 100, "reload_cycles": 20}
    result.update({"native_size": original.size, "scale": original.info["scale"]})
    (artifacts / "enhancement-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    (artifacts / "support.txt").write_text(str(bus.get_object("org.kde.KWin", "/KWin").supportInformation(dbus_interface="org.kde.KWin")))
finally:
    client.terminate()
    client.wait(timeout=10)
