#!/usr/bin/env python3
"""Exercise only the compositor/private bus created by scripts/run-nested.sh."""
import os
import configparser
from pathlib import Path
import tempfile
import time

import dbus
from PIL import Image

if os.environ.get("KWIN_VFX_NESTED") != "1":
    raise SystemExit("Run through scripts/run-nested.sh; never on the production bus")

artifacts = Path(os.environ["KWIN_VFX_RUN_DIR"])
bus = dbus.SessionBus()
effects = dbus.Interface(bus.get_object("org.kde.KWin", "/Effects"), "org.kde.kwin.Effects")
scripting = dbus.Interface(bus.get_object("org.kde.KWin", "/Scripting"), "org.kde.kwin.Scripting")


def configure(strength=0.6, denoise=0.5, sharpen=0.35, nvidia_mode=None, nvidia_sharpen=None, film_grain=None, cas_only=None, cas_strength=None, true_hdr=None, hdr_peak=None, true_hdr_sdr=None):
    path = Path(os.environ["XDG_CONFIG_HOME"]) / "kwinrc"
    config = configparser.ConfigParser()
    config.optionxform = str
    config.read(path)
    config["Effect-kwin_vfx"] = {"Strength": str(strength), "Denoise": str(denoise), "Sharpen": str(sharpen)}
    if nvidia_mode is not None:
        config["Effect-kwin_vfx"]["NvidiaMode"] = str(nvidia_mode)
    if nvidia_sharpen is not None:
        config["Effect-kwin_vfx"]["NvidiaSharpen"] = str(nvidia_sharpen)
    if film_grain is not None:
        config["Effect-kwin_vfx"]["FilmGrain"] = str(film_grain)
    if cas_only is not None:
        config["Effect-kwin_vfx"]["CasOnly"] = str(bool(cas_only)).lower()
    if cas_strength is not None:
        config["Effect-kwin_vfx"]["CasStrength"] = str(cas_strength)
    if true_hdr is not None:
        config["Effect-kwin_vfx"]["TrueHdr"] = str(bool(true_hdr)).lower()
    if hdr_peak is not None:
        config["Effect-kwin_vfx"]["HdrPeakNits"] = str(hdr_peak)
    if true_hdr_sdr is not None:
        config["Effect-kwin_vfx"]["TrueHdrSdr"] = str(bool(true_hdr_sdr)).lower()
    with path.open("w") as file:
        config.write(file, space_around_delimiters=False)
    effects.reconfigureEffect("kwin_vfx")
    time.sleep(0.05)


def script(code):
    path = artifacts / f"script-{time.monotonic_ns()}.js"
    path.write_text(code)
    name = path.stem
    number = scripting.loadScript(str(path), name, signature="ss")
    assert number >= 0
    dbus.Interface(bus.get_object("org.kde.KWin", f"/Scripting/Script{number}"), "org.kde.kwin.Script").run()
    scripting.unloadScript(name)


def screenshot(name):
    interface = dbus.Interface(bus.get_object("org.kde.KWin.ScreenShot2", "/org/kde/KWin/ScreenShot2"),
                               "org.kde.KWin.ScreenShot2")
    with tempfile.TemporaryFile() as file:
        info = interface.CaptureScreen(os.environ.get("KWIN_VFX_TEST_OUTPUT", "WL-0"),
            dbus.Dictionary({"hide-caller-windows": False, "native-resolution": True}, signature="sv"),
            dbus.types.UnixFd(file.fileno()))
        # CaptureScreen can return metadata before its asynchronous FD writer
        # completes. Do not seek the shared descriptor while it is still writing.
        expected = int(info["stride"]) * int(info["height"])
        deadline = time.monotonic() + 10
        while os.fstat(file.fileno()).st_size < expected:
            if time.monotonic() >= deadline:
                raise TimeoutError("KWin screenshot writer did not finish")
            time.sleep(0.01)
        file.seek(0)
        data = file.read(expected)
    width, height, stride, fmt = (int(info[key]) for key in ("width", "height", "stride", "format"))
    if fmt not in (4, 5, 6, 16, 17, 18):
        raise AssertionError(f"Unsupported screenshot QImage format {fmt}")
    layout = "BGRA" if fmt in (4, 5, 6) else "RGBA"
    image = Image.frombytes("RGBA", (width, height), data, "raw", layout, stride).convert("RGB")
    image.info["scale"] = float(info["scale"])
    image.save(artifacts / f"{name}.png")
    return image


def pixel(image, x, y):
    scale = image.info["scale"]
    return image.getpixel((round(x * scale), round(y * scale)))


def same(a, b):
    return max(abs(x - y) for x, y in zip(a, b)) <= 3


def toggle():
    accel.invokeShortcut("ToggleWindowEnhancement", "default", signature="ss")
    time.sleep(0.04)


accel = dbus.Interface(bus.get_object("org.kde.kglobalaccel", "/component/kwin"), "org.kde.kglobalaccel.Component")
