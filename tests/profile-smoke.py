#!/usr/bin/env python3
"""Native capture+draw CPU submission timings on a changing fullscreen window."""
import json
import os
from pathlib import Path
import subprocess
import time
from nested_support import artifacts, effects, script, configure, screenshot, toggle

client = subprocess.Popen([str(Path(os.environ["KWIN_VFX_BUILD_DIR"]) / "kwin_vfx_smoke_client")],
    env=dict(os.environ, KWIN_VFX_TEST_PATTERN="1", KWIN_VFX_ANIMATE="1"))
try:
    time.sleep(1)
    script("""
        for (let w of workspace.windowList()) {
            if (w.caption === 'VFX test control') w.closeWindow();
            if (w.caption === 'VFX test target') { workspace.activeWindow = w; w.fullScreen = true; }
        }
    """)
    configure()
    toggle()
    time.sleep(0.3)
    screenshot("profile-fullscreen")
    effects.debug("kwin_vfx", "reset", signature="ss")
    time.sleep(float(os.environ.get("KWIN_VFX_PROFILE_SECONDS", "3")))
    result = json.loads(effects.debug("kwin_vfx", "", signature="ss"))
    assert len(result["profile"]["samples"]) >= 20
    assert all(sample["success"] for sample in result["profile"]["samples"])
    (artifacts / "profile-result.json").write_text(json.dumps(result, indent=2) + "\n")
    print({k:v for k,v in result['profile'].items() if k != 'samples'}, "samples",len(result['profile']['samples']),flush=True)
finally:
    client.terminate()
    client.wait(timeout=10)
