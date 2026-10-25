#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Click the real tray DBus menu; all effect commands stay on a private bus."""
import json
import configparser
import os
from pathlib import Path
import subprocess
import time
import dbus
from nested_support import artifacts, bus, effects, script, configure

desktop = dbus.bus.BusConnection(os.environ["KWIN_VFX_TEST_TRAY_BUS"])
watcher = dbus.Interface(desktop.get_object("org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher"), "org.freedesktop.DBus.Properties")
desktop_bus_api = dbus.Interface(desktop.get_object("org.freedesktop.DBus", "/org/freedesktop/DBus"), "org.freedesktop.DBus")
build = Path(os.environ["KWIN_VFX_BUILD_DIR"])
client = subprocess.Popen([str(build/"kwin_vfx_smoke_client")], env=dict(os.environ,KWIN_VFX_TEST_PATTERN="1"))
tray = None
try:
    time.sleep(1)
    configure(strength=1,cas_only=True,cas_strength=.5,film_grain=0,true_hdr=True)
    effects.unloadEffect("kwin_vfx")
    config_path=Path(os.environ["XDG_CONFIG_HOME"])/"kwinrc"
    config=configparser.ConfigParser();config.optionxform=str;config.read(config_path)
    config["Effect-kwin_vfx"].update({"NvidiaEnabled":"true","NvidiaRuntime":str(Path("build-nvidia/runtime").resolve())})
    with config_path.open("w") as file:config.write(file,space_around_delimiters=False)
    assert effects.loadEffect("kwin_vfx")
    script("workspace.activeWindow=workspace.windowList().find(w=>w.caption==='VFX test control');")
    tray = subprocess.Popen([str(build/"kwin_vfx_preview_tray"),"--preview-session",str(artifacts/"session.json")],
        env=dict(os.environ,DBUS_SESSION_BUS_ADDRESS=os.environ["KWIN_VFX_TEST_TRAY_BUS"],WAYLAND_DISPLAY=os.environ["KWIN_VFX_TEST_TRAY_DISPLAY"]))
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        found=None
        for item in watcher.Get("org.kde.StatusNotifierWatcher","RegisteredStatusNotifierItems"):
            name,sep,path=str(item).partition("/")
            try:
                owner_pid=desktop_bus_api.GetConnectionUnixProcessID(name)
                if owner_pid != tray.pid:continue
                props=dbus.Interface(desktop.get_object(name,"/"+path if sep else "/StatusNotifierItem"),"org.freedesktop.DBus.Properties")
                menu_path=str(props.Get("org.kde.StatusNotifierItem","Menu"))
                found=dbus.Interface(desktop.get_object(name,menu_path),"com.canonical.dbusmenu")
                break
            except dbus.DBusException:pass
        if found is not None:break
        assert tray.poll() is None,"Tray exited"
        time.sleep(.1)
    assert found is not None,"Tray did not register"
    menu=found
    control=dbus.Interface(bus.get_object("org.kde.KWin","/WindowEnhancement"),"org.kde.KWin.WindowEnhancement")
    def state():return json.loads(effects.debug("kwin_vfx","",signature="ss"))
    def nodes():
        _,root=menu.GetLayout(0,-1,["label","enabled","toggle-state"],signature="iias")
        def flatten(node):return [node]+[n for child in node[2] for n in flatten(child)]
        return flatten(root)
    def action(prefix):return next(n for n in nodes() if str(n[1].get("label","")).startswith(prefix))
    def click(prefix):
        n=action(prefix);assert n[1].get("enabled",True),prefix
        menu.Event(dbus.Int32(n[0]),"clicked",dbus.Int32(0,variant_level=1),dbus.UInt32(0),signature="isvu")
        time.sleep(.4)
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        if any(str(n[1].get("label","")).startswith("VFX test target") for n in nodes()):break
        time.sleep(.1)
    if not state()["hdr_available"]:
        hdr=action("TrueHDR")
        assert not hdr[1].get("enabled",True),"HDR should be gray on SDR"
        assert not state()["true_hdr"] and state()["true_hdr_requested"]
    click("VFX test target")
    listed=json.loads(control.windowList())
    assert next(w for w in listed if w["title"]=="VFX test target")["enabled"]
    assert next(w for w in listed if w["title"]=="VFX test control")["active"],"Tray selection changed window focus"
    click("VFX test control")
    assert state()["selected"]==2
    assert action("VFX test target")[1]["toggle-state"]==1
    if not state()["hdr_available"]:
        click("TrueHDR look — SDR tone mapping")
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            value=state();assert not value["nvidia_error"],value
            if value["nvidia_draws"]>0 and len(value["nvidia_jobs"])==2 and all(j["true_hdr"] for j in value["nvidia_jobs"]):break
            time.sleep(.1)
        else:raise AssertionError(state())
        assert value["true_hdr_sdr"] and not value["true_hdr_output"] and not value["hdr_output_seen"]
        click("TrueHDR look — SDR tone mapping")
        assert not state()["true_hdr"] and not state()["true_hdr_sdr"]
    click("VFX test target")
    assert state()["selected"]==1
    click("Pause all enhancements")
    assert not state()["processing"] and state()["selected"]==1
    click("Resume checked windows")
    assert state()["processing"] and state()["selected"]==1
    click("Disable all windows")
    assert state()["selected"]==0
    script("workspace.windowList().find(w=>w.caption==='VFX test target').closeWindow();")
    time.sleep(.5)
    assert not any(str(n[1].get("label","")).startswith("VFX test target") for n in nodes())
    result={"passed":True,"tray_window_checkboxes":True,"same_app_independent":True,"selection_preserves_focus":True,
        "checkbox_state_tracks_effect":True,"pause_resume":True,"disable_all":True,"closed_windows_removed":True,
        "hdr_grayed_on_sdr":not state()["hdr_available"],"sdr_hdr_look_optional":True}
    (artifacts/"window-tray-result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)
finally:
    if tray is not None:tray.terminate();tray.wait(timeout=10)
    client.terminate();client.wait(timeout=10)
