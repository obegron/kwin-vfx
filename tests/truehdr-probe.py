#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone TrueHDR feasibility; never imported by the KWin effect."""
import argparse
import json
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime-root", type=Path, required=True)
parser.add_argument("--input", type=Path, required=True)
parser.add_argument("--output-dir", type=Path, required=True)
parser.add_argument("--luminance", type=int, default=650)
args = parser.parse_args()
assert 400 <= args.luminance <= 2000
root = args.runtime_root.resolve()
assert "Version: 0.2.0.0" in (root / "nvidia_vfx-0.2.0.0.dist-info/METADATA").read_text()
destination = args.output_dir.resolve()
destination.mkdir(parents=True, exist_ok=True)
report = {"scope": "Standalone wheel TrueHDR correctness probe; CPU transfers confined to this harness. No KWin/HDR presentation validation.",
          "luminance": args.luminance, "webcam_denoiser_included": False}
status = 0
try:
    import numpy as np
    import torch
    from PIL import Image
    sys.path.insert(0, str(root))
    from nvvfx import TrueHDR

    image = Image.open(args.input).convert("RGB")
    pixels = np.asarray(image).copy()
    source = torch.from_numpy(pixels).permute(2, 0, 1).to(device="cuda", dtype=torch.float32) / 255
    # The SDK expects full-range Rec.709/D65 gamma 2.2, rather than the
    # piecewise sRGB transfer used for this PNG fixture.
    linear = torch.where(source <= 0.04045, source / 12.92, ((source + 0.055) / 1.055).pow(2.4))
    source = linear.pow(1 / 2.2).contiguous()
    torch.cuda.synchronize()
    with TrueHDR(luminance=args.luminance) as effect:
        result = effect.run(source, non_blocking=False, stream_ptr=0)
        output = torch.from_dlpack(result.image).clone()
        torch.cuda.synchronize()  # Own the pixels before SDK teardown.
        raw = output.cpu().numpy()
    assert raw.dtype == np.uint32 and raw.shape == (image.height, image.width), (raw.dtype, raw.shape)
    channels = np.stack((raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023), axis=-1)
    assert channels.max() > channels.min(), "TrueHDR output is flat"
    raw.astype("<u4", copy=False).tofile(destination / "truehdr.rgb10a2")
    np.save(destination / "truehdr.npy", raw)
    report.update({"passed": True, "gpu": torch.cuda.get_device_name(), "size": [image.width, image.height],
                   "dtype": str(raw.dtype), "rgb10_min": int(channels.min()), "rgb10_max": int(channels.max()),
                   "output_encoding": "R10G10B10A2, BT.2020 primaries, ST2084/PQ",
                   "contrast": 100, "saturation": 100, "middle_gray": 50, "debanding_off": 0})
except Exception as error:
    report.update({"passed": False, "error": str(error)})
    status = 1
(destination / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report), flush=True)
raise SystemExit(status)
