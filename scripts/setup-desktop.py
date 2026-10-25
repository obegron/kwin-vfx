#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Per-user setup for a source-built or pacman-installed Window Enhancement."""
import argparse
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess

VERSION = "0.2.0.0"
GROUP = "Effect-kwin_vfx"
MARKER = "# Managed by kwin-vfx-setup"


def config_home():
    return Path(os.environ.get("XDG_CONFIG_HOME",str(Path.home()/".config")))


def read_setting(key, default="", group=GROUP):
    result=subprocess.run(["kreadconfig6","--file","kwinrc","--group",group,"--key",key,"--default",default],capture_output=True,text=True,check=False)
    return result.stdout.strip() if result.returncode in (0,1) else default


def runtime_ready(path):
    path=Path(path)
    metadata=path/f"nvidia_vfx-{VERSION}.dist-info/METADATA"
    return metadata.is_file() and f"\nVersion: {VERSION}\n" in metadata.read_text() and (path/"nvvfx/libs/libVideoFX.so").is_file()


def installation(path=None):
    path=Path(path) if path else Path(__file__).resolve().parents[1]/"share/kwin-vfx/installation.json"
    if not path.is_file():raise ValueError("Install with CMake or makepkg first; installation.json is missing")
    return json.loads(path.read_text())


def enable_plan(info, args):
    default_runtime=Path(os.environ.get("XDG_DATA_HOME",str(Path.home()/".local/share")))/f"kwin-vfx/runtime/nvidia-vfx-{VERSION}"
    existing=read_setting("NvidiaRuntime") or read_setting("NvidiaRuntime",group="Effect-kwin_vsr")
    preferred=default_runtime if runtime_ready(default_runtime) else existing if existing and runtime_ready(existing) else default_runtime
    runtime=Path(args.runtime_root or preferred).absolute()
    ready=runtime_ready(runtime)
    if args.runtime_root and not ready:raise ValueError(f"Native runtime missing at {runtime}")
    sdk=info["nvidia_backend"] and ready
    defaults={"NvidiaMode":"12","Strength":"0.6","CasStrength":"0.5","FilmGrain":"0",
              "TrueHdr":"false","TrueHdrSdr":"false","HdrPeakNits":"650","CasOnly":"false" if sdk else "true"}
    settings={key:read_setting(key,value,group="Effect-kwin_vsr") for key,value in defaults.items()
              if read_setting(key,"__missing__")=="__missing__"}
    for key in ("Denoise","Sharpen"):
        old=read_setting(key,"__missing__",group="Effect-kwin_vsr")
        if old!="__missing__" and read_setting(key,"__missing__")=="__missing__":settings[key]=old
    settings["NvidiaEnabled"]="true" if sdk else "false"
    if sdk:settings["NvidiaRuntime"]=str(runtime)
    else:
        settings.update({"CasOnly":"true","TrueHdr":"false","TrueHdrSdr":"false"})
    user_path=args.user_plugin_path or info.get("user_plugin_path",False)
    plan={"settings":settings,"runtime_ready":ready,"nvidia_enabled":sdk,"runtime":str(runtime),
          "autostart":not args.no_autostart,"plugin_root":info["plugin_root"],"requires_login":user_path,
          "legacy_settings_migrated":bool(read_setting("Strength",group="Effect-kwin_vsr"))}
    if user_path:
        # Scope this to KWin; a session-wide QT_PLUGIN_PATH can interfere with
        # unrelated applications and other Qt installations.
        paths=list(dict.fromkeys([info["plugin_root"],*filter(None,os.environ.get("QT_PLUGIN_PATH","").split(":"))]))
        value=":".join(paths).replace("%","%%").replace("\\","\\\\").replace('"','\\"')
        plan["service_override"]=MARKER+'\n[Service]\nEnvironment="QT_PLUGIN_PATH='+value+'"\n'
    return plan


def write_setting(group,key,value):
    subprocess.run(["kwriteconfig6","--file","kwinrc","--group",group,"--key",key,value],check=True)


def desktop_entry(binary):
    quoted=str(binary).replace("\\","\\\\").replace('"','\\"').replace("`","\\`").replace("$","\\$")
    return "\n".join(["[Desktop Entry]","Type=Application","Name=KWin VFX",
        "Comment=Choose windows to enhance from the system tray",f'Exec="{quoted}" --desktop',
        "Icon=preferences-desktop-display","Terminal=false","StartupNotify=false","X-KWin-VSR-Managed=true",""])


def connection():
    try:
        import dbus
        bus=dbus.SessionBus()
        object=bus.get_object("org.kde.KWin","/Effects")
        return dbus.Interface(object,"org.kde.kwin.Effects"),dbus.Interface(object,"org.freedesktop.DBus.Properties")
    except ImportError as error:raise ValueError("Desktop setup requires python-dbus") from error
    except dbus.DBusException as error:raise ValueError(f"KWin's session bus is unavailable: {error}") from error


def apply_enable(info,plan):
    home=config_home();home.mkdir(parents=True,exist_ok=True)
    service=home/"systemd/user/plasma-kwin_wayland.service.d/60-kwin-vfx.conf"
    if "service_override" in plan and service.exists() and not service.read_text().startswith(MARKER):
        raise ValueError(f"A different service override already exists: {service}")
    path=home/"kwinrc"
    if path.exists():
        backup=home/("kwinrc.before-window-enhancement-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
        shutil.copy2(path,backup);backup.chmod(0o600)
    for key,value in plan["settings"].items():write_setting(GROUP,key,value)
    if "service_override" in plan:
        service.parent.mkdir(parents=True,exist_ok=True);service.write_text(plan["service_override"])
        subprocess.run(["systemctl","--user","daemon-reload"],check=True)
    if plan["autostart"]:
        autostart=home/"autostart/kwin-vfx-tray.desktop"
        autostart.parent.mkdir(parents=True,exist_ok=True)
        autostart.write_text(desktop_entry(Path(info["bindir"])/"kwin-vfx-tray"))
    old_autostart=home/"autostart/kwin-vsr-tray.desktop"
    if old_autostart.exists() and "kwin-vsr-tray" in old_autostart.read_text():
        old_autostart.rename(old_autostart.with_suffix(".desktop.before-kwin-vfx-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S")))
    write_setting("Plugins","kwin_vsrEnabled","false")
    write_setting("Plugins","kwin_vfxEnabled","true")
    if plan["requires_login"]:
        print("Installed for your user. Log out and back in once so KWin sees the user plugin path.")
    else:
        effects,properties=connection()
        loaded=properties.Get("org.kde.kwin.Effects","loadedEffects")
        if "kwin_vsr" in loaded:
            print("Legacy kwin-vsr is still running. Settings migrated; log out and back in to switch to KWin VFX without interrupting it.")
        elif "kwin_vfx" in loaded:
            print("Effect already loaded. Use a fresh login for a rebuilt plugin; the running compositor was not restarted.")
        elif effects.loadEffect("kwin_vfx"):
            print("Window Enhancement loaded. Start kwin-vfx-tray --desktop and check the windows you want.")
        else:
            write_setting("Plugins","kwin_vfxEnabled","false")
            raise ValueError("KWin declined the plugin. Rebuild for the installed KWin version and log in again.")
    if not plan["nvidia_enabled"]:
        print("CAS/shader mode enabled. For AI: kwin-vfx-runtime --download, then kwin-vfx-setup enable.")


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action",choices=("status","enable","disable"),nargs="?",default="status")
    parser.add_argument("--runtime-root",type=Path)
    parser.add_argument("--user-plugin-path",action="store_true",help="Register the installed user plugin path for KWin's next login")
    parser.add_argument("--no-autostart",action="store_true")
    parser.add_argument("--dry-run",action="store_true",help="Print setup without writing settings or contacting KWin")
    parser.add_argument("--installation",type=Path,help=argparse.SUPPRESS)
    args=parser.parse_args()
    try:
        if args.action=="disable":
            if args.dry_run:print("Would disable/unload the effect and remove its managed tray autostart.");return 0
            write_setting("Plugins","kwin_vfxEnabled","false")
            effects,_=connection();effects.unloadEffect("kwin_vfx")
            autostart=config_home()/"autostart/kwin-vfx-tray.desktop"
            if autostart.exists() and "X-KWin-VSR-Managed=true" in autostart.read_text():autostart.unlink()
            print("Window Enhancement disabled. KWin was not restarted.");return 0
        info=installation(args.installation)
        plan=enable_plan(info,args)
        if args.action=="status" or args.dry_run:
            print(json.dumps({"installation":info,**plan},indent=2));return 0
        if os.environ.get("KWIN_VFX_NESTED")=="1":raise ValueError("Run desktop setup outside the nested launcher")
        apply_enable(info,plan)
    except (OSError,ValueError,subprocess.CalledProcessError) as error:
        parser.exit(1,f"{error}\n")
    return 0


if __name__=="__main__":raise SystemExit(main())
