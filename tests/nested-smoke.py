#!/usr/bin/env python3
"""Window selection and tint checks; run only through scripts/run-nested.sh."""
import json
import os
from pathlib import Path
import subprocess
import time
import dbus
from nested_support import artifacts, bus, effects, script, screenshot, pixel, same, toggle

if not os.environ.get("KWIN_VFX_TEST_TINT"):
    raise SystemExit("Tint test requires KWIN_VFX_TEST_TINT=1; use enhancement-smoke.py for filtering")

client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")])
try:
    time.sleep(1)
    script("""
        let windows = workspace.windowList();
        let target = windows.find(w => w.caption === 'VFX test target');
        let control = windows.find(w => w.caption === 'VFX test control');
        if (!target || !control) throw new Error('Test windows missing');
        target.frameGeometry = {x: 60, y: 80, width: 360, height: 240};
        control.frameGeometry = {x: 520, y: 80, width: 360, height: 240};
        workspace.activeWindow = target;
    """)
    time.sleep(0.3)
    assert effects.isEffectLoaded("kwin_vfx")
    accel = dbus.Interface(bus.get_object("org.kde.kglobalaccel", "/component/kwin"), "org.kde.kglobalaccel.Component")
    (artifacts / "shortcuts.xml").write_text(str(bus.get_object("org.kde.kglobalaccel", "/component/kwin").Introspect(dbus_interface="org.freedesktop.DBus.Introspectable")))
    assert "ToggleWindowEnhancement" in accel.shortcutNames()
    baseline = screenshot("disabled")
    original_target = pixel(baseline, 90, 110)
    original_control = pixel(baseline, 550, 110)
    assert original_target[0] > 150 and same(original_target, original_control), (original_target, original_control)
    toggle()
    enabled = screenshot("enabled")
    target_color = pixel(enabled, 90, 110)
    control_color = pixel(enabled, 550, 110)
    failure = os.environ.get("KWIN_VFX_TEST_FAILURE", "")
    if failure:
        assert same(original_target, target_color), (failure, original_target, target_color)
    else:
        assert target_color[0] < original_target[0] - 50, (original_target, target_color)
        assert abs(target_color[1] - original_target[1]) < 5
    assert same(original_control, control_color), (original_control, control_color)
    if not failure:
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test control');")
        toggle()
        both = screenshot("both-enabled")
        assert pixel(both, 90, 110)[0] < 150
        assert pixel(both, 550, 110)[0] < 150
        toggle()
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test target');")
        for _ in range(100):
            toggle()
        assert pixel(screenshot("after-100-toggles"), 90, 110)[0] < 150
        script("""
            let w = workspace.activeWindow;
            w.frameGeometry = {x: 80, y: 360, width: 420, height: 280};
        """)
        time.sleep(0.2)
        moved = screenshot("moved-resized")
        assert pixel(moved, 110, 390)[0] < 150
        assert same(original_control, pixel(moved, 550, 110))
        script("workspace.activeWindow.frameGeometry = {x: 60, y: 80, width: 360, height: 240};")
        time.sleep(0.2)
        toggle()
    restored = screenshot("restored")
    assert same(original_target, pixel(restored, 90, 110))
    if not failure:
        toggle()
        script("workspace.activeWindow.closeWindow();")
        time.sleep(0.2)
        assert same(original_control, pixel(screenshot("closed-selected"), 550, 110))
        # Unload while another window is actively selected, then check restoration.
        script("workspace.activeWindow = workspace.windowList().find(w => w.caption === 'VFX test control');")
        toggle()
        effects.unloadEffect("kwin_vfx")
        assert same(original_control, pixel(screenshot("unloaded-selected"), 550, 110))
        assert effects.loadEffect("kwin_vfx")
    for _ in range(20):
        effects.unloadEffect("kwin_vfx")
        assert not effects.isEffectLoaded("kwin_vfx")
        assert effects.loadEffect("kwin_vfx")
    screenshot("reloaded")
    (artifacts / "result.json").write_text(json.dumps({
        "passed": True, "failure_mode": failure, "target_disabled": original_target,
        "target_enabled": target_color, "control_enabled": control_color, "reload_cycles": 20,
        "toggle_cycles": 100 if not failure else 0,
        "native_size": baseline.size, "scale": baseline.info["scale"],
    }, indent=2) + "\n")
    print((artifacts / "result.json").read_text(), flush=True)
    (artifacts / "support.txt").write_text(str(bus.get_object("org.kde.KWin", "/KWin").supportInformation(
        dbus_interface="org.kde.KWin")))
finally:
    client.terminate()
    client.wait(timeout=10)
