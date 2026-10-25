#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare a local browser fixture without altering or re-encoding its source."""
import argparse
import hashlib
import html
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("--output-dir", type=Path, required=True)
args = parser.parse_args()
source = args.source.resolve(strict=True)
dest = args.output_dir.resolve()
dest.mkdir(parents=True, exist_ok=True)
probe = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0",
    "-show_entries", "stream=width,height,r_frame_rate,codec_name", "-show_entries", "format=duration",
    "-of", "json", str(source)], text=True))
stream = probe["streams"][0]
video = dest / ("preview-input" + source.suffix.lower())
if source != video:
    shutil.copy2(source, video)
metadata = {"source": str(source), "sha256": hashlib.file_digest(source.open("rb"), "sha256").hexdigest(),
            "video": stream, "duration": probe["format"]["duration"], "reencoded": False}
(dest / "preview-source.json").write_text(json.dumps(metadata, indent=2) + "\n")
name = html.escape(source.name)
page = """<!doctype html><meta charset="utf-8"><title>Window enhancement preview</title>
<style>
body{margin:0;padding:24px;background:#24262b;color:#eee;font:18px sans-serif}
main{max-width:1280px;margin:auto}h1{font-size:26px;margin-top:0}
video{display:block;width:100%;max-height:72vh;background:#000;margin:18px 0}
p{line-height:1.5}kbd{background:#41454e;padding:3px 7px;border-radius:4px}
</style><main>
<h1>Window enhancement preview</h1>
<p>Focus this browser and toggle enhancement with <kbd>Meta</kbd> + <kbd>Ctrl</kbd> + <kbd>E</kbd>.<br>
Adjust strength with <kbd>Meta</kbd> + <kbd>Ctrl</kbd> + <kbd>PageUp</kbd> / <kbd>PageDown</kbd>.</p>
<video controls autoplay muted loop src="VIDEO"></video>
<p>NAME · WIDTH×HEIGHT · FPS fps<br>Use the video’s fullscreen button for a larger comparison.</p>
</main>"""
numerator, denominator = map(int, stream["r_frame_rate"].split("/"))
fps = numerator / denominator if denominator else 0
page = page.replace("VIDEO", html.escape(video.name, quote=True)).replace("NAME", name)
page = page.replace("WIDTH", str(stream["width"])).replace("HEIGHT", str(stream["height"])).replace("FPS", f"{fps:g}")
(dest / "preview.html").write_text(page)
print(dest / "preview.html")
