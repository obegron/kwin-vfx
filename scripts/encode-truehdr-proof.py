#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Encode the standalone SDK probe and its SDR reference for HDR-aware playback."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--probe-dir", type=Path, required=True)
parser.add_argument("--sdr-input", type=Path, required=True)
args = parser.parse_args()
directory = args.probe_dir.resolve()
result = json.loads((directory / "result.json").read_text())
assert result["passed"]
width, height = result["size"]
assert (directory / "truehdr.rgb10a2").stat().st_size == width * height * 4
hdr = directory / "truehdr-proof.mp4"
sdr = directory / "original-sdr.mp4"
subprocess.run(["ffmpeg", "-v", "error", "-stream_loop", "-1", "-f", "rawvideo",
    "-pixel_format", "x2bgr10le", "-video_size", f"{width}x{height}", "-framerate", "1",
    "-i", str(directory / "truehdr.rgb10a2"), "-vf",
    "scale=out_color_matrix=bt2020:in_range=full:out_range=tv,format=yuv420p10le,setparams=color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc:range=limited",
    "-c:v", "libx265", "-preset", "fast", "-crf", "12", "-x265-params",
    "log-level=error:colorprim=9:transfer=16:colormatrix=9", "-metadata", f"title=TrueHDR SDK proof ({result['luminance']} nit target)",
    "-t", "4", "-y", str(hdr)], check=True)
subprocess.run(["ffmpeg", "-v", "error", "-loop", "1", "-framerate", "1", "-i", str(args.sdr_input),
    "-vf", "scale=out_color_matrix=bt709:in_range=full:out_range=tv,format=yuv420p,setparams=color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709:range=limited",
    "-c:v", "libx264", "-preset", "fast", "-crf", "12", "-metadata", "title=Original SDR reference",
    "-t", "4", "-y", str(sdr)], check=True)
metadata = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_entries",
    "stream=width,height,pix_fmt,color_primaries,color_transfer,color_space", "-of", "json", str(hdr)]))["streams"][0]
assert metadata["color_transfer"] == "smpte2084" and metadata["color_primaries"] == "bt2020", metadata
assert metadata["pix_fmt"] == "yuv420p10le", metadata
(directory / "presentation-inputs.json").write_text(json.dumps({"passed": True, "hdr_stream": metadata,
    "scope": "Repeated still-frame proof, independently encoded SDR/HDR references; not live browser enhancement or a quality ranking."}, indent=2) + "\n")
print(json.dumps({"passed": True, "sdr": str(sdr), "hdr": str(hdr)}))
