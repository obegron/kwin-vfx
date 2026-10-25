#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stale output, nonblocking window disable and resize/model debounce."""
import json
import os
from pathlib import Path
import subprocess
import time
from nested_support import artifacts, effects, script, toggle, configure, screenshot, accel

assert os.environ.get("KWIN_VFX_TEST_NVIDIA_DELAY")
assert os.environ.get("KWIN_VFX_TEST_NVIDIA_TEARDOWN_DELAY")


def state():
    return json.loads(effects.debug("kwin_vfx", "", signature="ss"))


def wait(predicate):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        current = state()
        assert not current["nvidia_error"], current
        if predicate(current):
            return current
        time.sleep(0.01)
    raise AssertionError(state())


client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
                          env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1", KWIN_VFX_ANIMATE="1"))
try:
    time.sleep(1)
    script("workspace.activeWindow=workspace.windowList().find(w=>w.caption==='VFX test target');")
    configure(strength=1, nvidia_sharpen=0.5)
    screenshot("retirement-before")
    toggle()
    wait(lambda d: d["stale_ai_outputs"] > 0)
    wait(lambda d: d["nvidia_jobs"] and d["nvidia_jobs"][0]["state"] == 3)
    started = time.monotonic()
    accel.invokeShortcut("ToggleWindowEnhancement", "default", signature="ss")
    disabled = wait(lambda d: d["selected"] == 0)
    disable_ms = (time.monotonic() - started) * 1000
    assert disable_ms < 200, disable_ms  # Teardown is deliberately held for 250 ms.
    assert disabled["retiring_ai_slots"] > 0, disabled
    responses = 0
    max_response_ms = 0
    while True:
        start = time.monotonic()
        current = state()
        max_response_ms = max(max_response_ms, (time.monotonic() - start) * 1000)
        if not current["retiring_ai_slots"]:
            break
        responses += 1
        assert time.monotonic() - started < 10
        time.sleep(0.01)
    assert responses >= 8 and max_response_ms < 200, (responses, max_response_ms)
    toggle()
    wait(lambda d: bool(d["nvidia_jobs"]))
    starts = state()["ai_slot_starts"]
    for index in range(10):
        script(f"workspace.activeWindow.frameGeometry={{x:60,y:80,width:{400+index*8},height:{300+index*4}}};")
        time.sleep(0.03)
    assert state()["ai_slot_starts"] == starts, state()
    wait(lambda d: d["ai_slot_starts"] == starts + 1 and d["nvidia_jobs"])
    toggle()
    wait(lambda d: d["retiring_ai_slots"] == 0)
    result = {"passed": True, "stale_output_rejected": True, "disable_ms": disable_ms,
              "debug_responses_during_teardown": responses, "max_debug_response_ms": max_response_ms,
              "teardown_delay_ms": 250, "rapid_resize_model_loads": 0, "stable_resize_model_loads": 1}
    (artifacts / "retirement-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
