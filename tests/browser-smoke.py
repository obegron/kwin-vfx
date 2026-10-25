#!/usr/bin/env python3
"""Firefox + fixed local HTTP video integration in the isolated nested session."""
import functools
import hashlib
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import threading
import time
from datetime import datetime
from urllib.parse import parse_qs, urlparse
from PIL import ImageChops

from nested_support import artifacts, bus, effects, script, screenshot, pixel, same, toggle, configure

build_dir = Path(os.environ["KWIN_VFX_BUILD_DIR"])
frames = {}


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        query = urlparse(self.path)
        if query.path == "/frames":
            values = parse_qs(query.query)
            frames[values["source"][0]] = int(values["count"][0])
            self.send_response(204)
            self.end_headers()
        else:
            super().do_GET()

    def log_message(self, *args):
        pass


server = ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(build_dir)))
threading.Thread(target=server.serve_forever, daemon=True).start()
client = subprocess.Popen([str(build_dir / "kwin_vfx_smoke_client")])
results = []
test_tint = bool(os.environ.get("KWIN_VFX_TEST_TINT"))
gpu_log = None
gpu_sampler = None
if os.environ.get("KWIN_VFX_GPU_METRICS"):
    gpu_log = (artifacts / "gpu.csv").open("w")
    gpu_sampler = subprocess.Popen(["nvidia-smi",
        "--query-gpu=timestamp,utilization.gpu,utilization.memory,memory.used,power.draw",
        "--format=csv,noheader,nounits", "--loop-ms=200"], stdout=gpu_log, stderr=subprocess.DEVNULL)


class Marionette:
    """Minimal Firefox test connection; no browser extension or production profile."""
    def __init__(self, port):
        deadline = time.monotonic() + 12
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except ConnectionRefusedError:
                assert time.monotonic() < deadline, "Marionette did not start"
                time.sleep(0.1)
        self.serial = 0
        self.receive()  # Protocol greeting.
        self.call("WebDriver:NewSession", {"capabilities": {}})

    def receive(self):
        length = b""
        while not length.endswith(b":"):
            chunk = self.socket.recv(1)
            assert chunk, "Marionette disconnected"
            length += chunk
        remaining = int(length[:-1])
        payload = b""
        while remaining:
            chunk = self.socket.recv(remaining)
            assert chunk, "Marionette disconnected"
            payload += chunk
            remaining -= len(chunk)
        return json.loads(payload)

    def call(self, name, arguments=None):
        self.serial += 1
        payload = json.dumps([0, self.serial, name, arguments or {}]).encode()
        self.socket.sendall(str(len(payload)).encode() + b":" + payload)
        response = self.receive()
        assert response[0] == 1 and response[1] == self.serial and response[2] is None, response
        return response[3]

    def select_video_tab(self):
        for handle in self.call("WebDriver:GetWindowHandles"):
            self.call("WebDriver:SwitchToWindow", {"handle": handle})
            if self.call("WebDriver:GetTitle")["value"] == "VFX Browser Test":
                return
        raise AssertionError("Test video tab missing")

    def execute(self, code):
        return self.call("WebDriver:ExecuteScript", {"script": code, "args": []})["value"]


def measure(automation):
    initial_metrics = json.loads(effects.debug("kwin_vfx", "reset", signature="ss"))
    quality_code = "const v=document.querySelector('video'), q=v.getVideoPlaybackQuality(); return {total:q.totalVideoFrames, dropped:q.droppedVideoFrames, time:v.currentTime, paused:v.paused, ended:v.ended};"
    before = automation.execute(quality_code)
    started = datetime.now().astimezone().isoformat()
    callbacks_before = dict(frames)
    seconds = float(os.environ.get("KWIN_VFX_MEASURE_SECONDS", "2"))
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        automation.select_video_tab()
        time.sleep(min(0.5, max(0, deadline - time.monotonic())))
    after = automation.execute(quality_code)
    quality_available = before["total"] > 0 or after["total"] > 0
    metrics = json.loads(effects.debug("kwin_vfx", "", signature="ss"))
    if metrics.get("backend") == "nvidia-experimental" and metrics["processing"]:
        assert not metrics["nvidia_error"], metrics
        assert (metrics["nvidia_draws"] > initial_metrics["nvidia_draws"]
                or metrics.get("stale_ai_outputs", 0) > initial_metrics.get("stale_ai_outputs", 0)), "No completed AI work during playback"
    assert sum(count - callbacks_before.get(source, 0) for source,count in frames.items()) > 0, "No video callbacks during measurement"
    return {"seconds": seconds, "video_frames": after["total"] - before["total"] if quality_available else None,
        "dropped_video_frames": after["dropped"] - before["dropped"] if quality_available else None,
        "playback_quality_available": quality_available, "effect": metrics,
        "video_before": before, "video_after": after,
        "callback_delta": {source:count - callbacks_before.get(source, 0) for source,count in frames.items()},
        "started": started, "finished": datetime.now().astimezone().isoformat()}

try:
    time.sleep(1)
    script("""
        for (let w of workspace.windowList()) {
            if (w.caption === 'VFX test target') w.closeWindow();
            if (w.caption === 'VFX test control') w.frameGeometry = {x: 1300, y: 80, width: 360, height: 240};
        }
    """)
    sources = {source: build_dir / f"{source}.webm" for source in ("720p", "1080p")}
    if os.environ.get("KWIN_VFX_BROWSER_CLIP"):
        original_clip = Path(os.environ["KWIN_VFX_BROWSER_CLIP"]).resolve()
        custom_clip = build_dir / ("browser-custom" + original_clip.suffix)
        if original_clip != custom_clip.resolve():
            shutil.copy2(original_clip, custom_clip)
        sources = {"custom": custom_clip}
    for source, clip in sources.items():
        assert clip.is_file(), f"Generate {clip} using the commands in docs/validation.md"
        page = build_dir / f"browser-{source}.html"
        page.write_text("""<!doctype html><title>VFX Browser Test</title>
<style>body {background:rgb(200,120,80);color:white;margin:48px;font:24px sans-serif}
video {display:block;width:720px;height:405px;margin-top:40px}</style>
<p>Browser text and UI: window enhancement</p><video autoplay muted loop src='SOURCE.webm'></video>
<script>
const v = document.querySelector('video'); let count = 0;
function frame() { count++; if (count % 10 === 0)
fetch('/frames?source=SOURCE&count=' + count); v.requestVideoFrameCallback(frame); }
v.requestVideoFrameCallback(frame);
</script>""".replace("SOURCE.webm", clip.name).replace("SOURCE", source))
        profile = artifacts / f"firefox-{source}"
        profile.mkdir()
        with socket.socket() as port_socket:
            port_socket.bind(("127.0.0.1", 0))
            automation_port = port_socket.getsockname()[1]
        (profile / "user.js").write_text('user_pref("media.autoplay.default", 0);\n'
            'user_pref("browser.shell.checkDefaultBrowser", false);\n'
            'user_pref("browser.aboutwelcome.enabled", false);\n'
            'user_pref("browser.startup.homepage_override.mstone", "ignore");\n'
            f'user_pref("marionette.port", {automation_port});\n')
        browser = subprocess.Popen(["firefox", "--no-remote", "--marionette", "--profile", str(profile),
            f"http://127.0.0.1:{server.server_port}/{page.name}"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            automation = Marionette(automation_port)
            time.sleep(1)
            deadline = time.monotonic() + 12
            while frames.get(source, 0) < 10:
                assert time.monotonic() < deadline, "Firefox did not present video frames"
                automation.select_video_tab()
                time.sleep(0.2)
            automation.select_video_tab()
            script("""
                let w = workspace.windowList().find(w => w.caption.includes('VFX Browser Test'));
                if (!w) throw new Error('Firefox test window missing');
                w.frameGeometry = {x: 60, y: 80, width: 1100, height: 800};
                workspace.activeWindow = w;
            """)
            time.sleep(0.3)
            if not test_tint:
                configure()
                automation.execute("document.querySelector('video').pause(); return true;")
                time.sleep(0.2)
            baseline = screenshot(f"browser-{source}-disabled")
            original = pixel(baseline, 90, 280)
            control = pixel(baseline, 1330, 110)
            assert original[0] > 150, original
            count_before = frames[source]
            toggle()
            time.sleep(0.5)
            if not test_tint:
                deadline = time.monotonic() + 20
                while True:
                    state = json.loads(effects.debug("kwin_vfx", "", signature="ss"))
                    if state.get("backend") != "nvidia-experimental" or (state["nvidia_jobs"] and all(j["state"] == 4 for j in state["nvidia_jobs"]) and (state["nvidia_draws"] > 0 or state.get("stale_ai_outputs", 0) > 0)):
                        break
                    assert not state["nvidia_error"] and time.monotonic() < deadline, state
                    time.sleep(0.05)
            enabled = screenshot(f"browser-{source}-enabled")
            changed = pixel(enabled, 90, 280)
            if test_tint:
                zero_metrics = None
                assert changed[0] < original[0] - 50, (original, changed)
            else:
                assert same(original, changed), "Filter changed constant browser background"
                scale = enabled.info["scale"]
                box = tuple(round(v * scale) for v in (120, 360, 800, 680))
                assert ImageChops.difference(baseline.crop(box), enabled.crop(box)).getbbox(), "Filter did not change video pixels"
                configure(strength=0)
                zero = screenshot(f"browser-{source}-zero")
                extrema = ImageChops.difference(baseline.crop(box), zero.crop(box)).getextrema()
                assert max(x[1] for x in extrema) <= 3, "Zero strength did not restore video"
                automation.execute("document.querySelector('video').play(); return true;")
                zero_metrics = measure(automation)
                assert not zero_metrics["effect"]["processing"]
                assert not zero_metrics["effect"].get("profile", {}).get("samples", [])
                configure()
                effects.debug("kwin_vfx", "reset", signature="ss")
                automation.execute("document.querySelector('video').play(); return true;")
            assert same(control, pixel(enabled, 1330, 110))
            deadline = time.monotonic() + 5
            while frames[source] <= count_before:
                assert time.monotonic() < deadline, "Video stopped presenting during tint"
                automation.select_video_tab()
                time.sleep(0.2)
            time.sleep(0.3)
            advancing = screenshot(f"browser-{source}-advancing")
            # The fixed page's video region must change in the rendered output.
            scale = enabled.info["scale"]
            video_box = tuple(round(v * scale) for v in (120, 360, 800, 680))
            assert ImageChops.difference(enabled.crop(video_box), advancing.crop(video_box)).getbbox(), "Enhanced video output froze"
            windowed_metrics = measure(automation)
            script("workspace.activeWindow.fullScreen = true;")
            time.sleep(0.3)
            fullscreen = screenshot(f"browser-{source}-fullscreen")
            assert fullscreen.size == baseline.size, fullscreen.size
            if test_tint:
                assert pixel(fullscreen, 30, 200)[0] < 150
            else:
                assert same(pixel(fullscreen, 30, 200), original)
            # Reset after the screenshot so metrics contain only ordinary paints.
            metrics = measure(automation)
            script("workspace.activeWindow.fullScreen = false;")
            time.sleep(0.3)
            toggle()
            restored = screenshot(f"browser-{source}-restored")
            assert same(original, pixel(restored, 90, 280))
            results.append({"source": source, "sha256": hashlib.sha256(clip.read_bytes()).hexdigest(),
                "disabled": original, "enabled": changed, "frames_presented": frames[source],
                "fullscreen": True, "windowed": True, "native_size": baseline.size,
                "scale": baseline.info["scale"], "output_frames_changed": True,
                "mode": "test_tint" if test_tint else windowed_metrics["effect"].get("backend", "spatial_filter"), "windowed_metrics": windowed_metrics,
                "fullscreen_metrics": metrics, "zero_strength_metrics": zero_metrics})
        finally:
            if "automation" in locals():
                automation.socket.close()
            browser.terminate()
            browser.wait(timeout=10)
    (artifacts / "browser-result.json").write_text(json.dumps(results, indent=2) + "\n")
    print((artifacts / "browser-result.json").read_text(), flush=True)
    (artifacts / "support.txt").write_text(str(bus.get_object("org.kde.KWin", "/KWin").supportInformation(
        dbus_interface="org.kde.KWin")))
finally:
    if gpu_sampler is not None:
        gpu_sampler.terminate()
        gpu_sampler.wait(timeout=10)
        gpu_log.close()
    server.shutdown()
    client.terminate()
    client.wait(timeout=10)
