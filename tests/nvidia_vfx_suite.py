#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reproduce isolated VFX still-frame measurements; never starts/modifies KWin."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SOURCE_SHA256 = "34bbd52a4b89fdf63c8ace50b268da26653a59508288100cd3c23de276db7931"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True,
                        help="Official Blender sintel_trailer-1080p.mp4; see docs/validation.md")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--python", default=sys.executable, help="Interpreter with torch and nvidia-vfx")
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    if sha256(args.source) != SOURCE_SHA256:
        parser.error("Source SHA256 differs from the pinned trailer")
    if args.frames < 1:
        parser.error("Frame count must be positive")
    dest = args.output_dir.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    source = str(args.source.resolve())

    def ffmpeg(*flags):
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, flags)], check=True)

    for height in (720, 1080):
        scale = ["-vf", "scale=1280:720:flags=lanczos"] if height == 720 else []
        ffmpeg("-ss", 20, "-i", source, "-t", 1, *scale, "-an", "-c:v", "libx264",
               "-crf", 36, "-preset", "medium", dest / f"sintel-{height}.mp4")
        ffmpeg("-ss", 0.5, "-i", dest / f"sintel-{height}.mp4", "-frames:v", 1, "-update", 1,
               dest / f"sintel-{height}.png")
    ffmpeg("-ss", 20.5, "-i", source, "-frames:v", 1, "-update", 1, dest / "sintel-reference.png")
    fixtures = {p.name: sha256(p) for p in dest.glob("sintel-*") if p.suffix in (".png", ".mp4")}
    manifest = {"source_url": "https://download.blender.org/durian/trailer/sintel_trailer-1080p.mp4",
                "source_sha256": SOURCE_SHA256, "reference_time_seconds": 20.5,
                "encoding": "libx264 CRF 36 preset medium; 720p uses Lanczos downsample",
                "ffmpeg": subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0],
                "fixtures": fixtures}
    (dest / "fixture-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    if args.prepare_only:
        return 0
    probe = Path(__file__).with_name("nvidia_vfx_probe.py")
    cases = [
        ("720-1080-high", 720, 1920, 1080, "HIGH", True),
        ("720-1080-bicubic", 720, 1920, 1080, "BICUBIC", True),
        ("720-4k-high", 720, 3840, 2160, "HIGH", False),
        ("1080-4k-high", 1080, 3840, 2160, "HIGH", False),
        ("1080-4k-bicubic", 1080, 3840, 2160, "BICUBIC", False),
        ("720-denoise", 720, 1280, 720, "DENOISE_HIGH", False),
        ("1080-denoise", 1080, 1920, 1080, "DENOISE_HIGH", True),
        ("1600-denoise", None, 2560, 1600, "DENOISE_HIGH", False),
        ("denoise_low-1600", None, 2560, 1600, "DENOISE_LOW", False),
        ("denoise_medium-1600", None, 2560, 1600, "DENOISE_MEDIUM", False),
        ("deblur_low-1600", None, 2560, 1600, "DEBLUR_LOW", False),
    ]
    results = []
    for name, height, width_out, height_out, mode, reference in cases:
        command = [args.python, str(probe), "--frames", str(args.frames), "--warmup", "10",
                   "--mode", mode, "--output-width", str(width_out), "--output-height", str(height_out),
                   "--output", str(dest / f"{name}.json"), "--image-output", str(dest / f"{name}.png")]
        if height:
            command += ["--input", str(dest / f"sintel-{height}.png")]
        else:
            command += ["--width", "2560", "--height", "1600"]
        if reference:
            command += ["--reference", str(dest / "sintel-reference.png")]
        if name == "720-1080-high":
            command += ["--baseline-output", str(dest / "bicubic-1080.png")]
        with (dest / f"{name}.log").open("w") as log:
            try:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=120)
                status = {"case": name, "exit_code": completed.returncode}
            except subprocess.TimeoutExpired:
                status = {"case": name, "exit_code": None, "error": "120 second timeout"}
        results.append(status)
        print(json.dumps(status), flush=True)
    passed = all(r["exit_code"] == 0 for r in results)
    (dest / "suite.json").write_text(json.dumps({"passed": passed, "cases": results}, indent=2) + "\n")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
