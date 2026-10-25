#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit live verification affecting only the two fixture windows it creates."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import dbus

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("--desktop",action="store_true",required=True)
parser.add_argument("--build-dir",type=Path,default=Path("build-desktop"))
args=parser.parse_args()
if os.environ.get("KWIN_VFX_NESTED")=="1":raise SystemExit("This is the explicit desktop check")
bus=dbus.SessionBus()
control=dbus.Interface(bus.get_object("org.kde.KWin","/WindowEnhancement"),"org.kde.KWin.WindowEnhancement")
effect=dbus.Interface(bus.get_object("org.kde.KWin","/Effects"),"org.kde.kwin.Effects")
def windows():return json.loads(control.windowList())
def state():return json.loads(effect.debug("kwin_vfx","",signature="ss"))
before={w["id"] for w in windows()}
own=[]
client=subprocess.Popen([str(args.build_dir.resolve()/"kwin_vfx_smoke_client")],env=dict(os.environ,KWIN_VFX_TEST_PATTERN="1",QT_QPA_PLATFORM="wayland"))
try:
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        own=[w for w in windows() if w["id"] not in before and w["title"] in ("VFX test target","VFX test control")]
        if len(own)==2:break
        time.sleep(.1)
    assert len(own)==2,"Fixture windows did not appear"
    baseline=state()["nvidia_draws"]
    for window in own:
        assert control.setWindowEnabled(window["id"],True)
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        current=state();assert not current["nvidia_error"],current
        if current["nvidia_draws"]>baseline and len(current["nvidia_jobs"])>=2 and all(j.get("published_revision",0)>0 for j in current["nvidia_jobs"]):break
        time.sleep(.1)
    else:raise AssertionError(state())
    ids={w["id"] for w in own}
    assert all(w["enabled"] for w in windows() if w["id"] in ids)
    assert control.setWindowEnabled(own[0]["id"],False)
    assert next(w for w in windows() if w["id"]==own[1]["id"])["enabled"]
    assert control.setWindowEnabled(own[1]["id"],False)
    result={"passed":True,"live_desktop_compositor":True,"fixture_windows_only":True,"two_independent_windows":True,
        "nvidia_draws":current["nvidia_draws"]-baseline,"deblur_mode":current["nvidia_mode"],
        "sdr_hdr_look":current["true_hdr_sdr"],"hdr_output_enabled":current["true_hdr_output"],
        "hdr_available":current["hdr_available"],"output_colors":current["output_colors"],
        "kwin_restarted":False}
    (args.build_dir/"desktop-smoke.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result,indent=2),flush=True)
finally:
    remaining={w["id"] for w in windows()}
    for window in own:
        if window["id"] in remaining:control.setWindowEnabled(window["id"],False)
    client.terminate();client.wait(timeout=10)
