#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone official VFX Python runtime gate; never imported into KWin.

Blocking execution and output cloning here are for correctness/measurement only.
They are not an asynchronous native compositor backend.
"""
import argparse
import importlib.metadata
import json
import math
from pathlib import Path
import statistics
import time
import traceback


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="Optional fixed RGB image for a comparison")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--output-width", type=int, default=1280)
    parser.add_argument("--output-height", type=int, default=720)
    parser.add_argument("--mode", default="HIGH")
    parser.add_argument("--strength", type=float, default=1)
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--image-output", type=Path)
    parser.add_argument("--baseline-output", type=Path)
    parser.add_argument("--reference", type=Path, help="Aligned RGB reference at the exact output size")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"passed": False, "stage": "imports", "arguments": {k:str(v) if isinstance(v,Path) else v for k,v in vars(args).items()}}
    try:
        import numpy as np
        from PIL import Image
        import torch
        import torch.nn.functional as functional
        import nvvfx

        if not 0 <= args.strength <= 1 or args.frames < 1 or args.warmup < 0:
            raise ValueError("Invalid strength/frame count")
        torch.cuda.set_device(0)
        report.update({"torch": torch.__version__, "torch_cuda": torch.version.cuda,
            "nvidia_vfx": importlib.metadata.version("nvidia-vfx"), "sdk": str(nvvfx.get_sdk_version()),
            "gpu": torch.cuda.get_device_name(0), "compute_capability": torch.cuda.get_device_capability(0)})
        report["stage"] = "input"
        if args.input:
            pixels = np.asarray(Image.open(args.input).convert("RGB")).copy()
            source = torch.from_numpy(pixels).to("cuda").permute(2, 0, 1).contiguous().float() / 255
        else:
            y, x = torch.meshgrid(torch.linspace(0, 1, args.height, device="cuda"),
                torch.linspace(0, 1, args.width, device="cuda"), indexing="ij")
            source = torch.stack((x, y, 0.3 + 0.4 * ((x * 20).floor() + (y * 20).floor()).remainder(2))).contiguous()
        report["input_shape"] = list(source.shape)
        mode = nvvfx.VideoSuperRes.QualityLevel[args.mode]
        report["mode_id"] = int(mode)
        torch.cuda.synchronize()
        report["device_free_before_load"] = torch.cuda.mem_get_info()[0]
        started = time.perf_counter()
        report["stage"] = "create_load"
        with nvvfx.VideoSuperRes(quality=mode, strength=args.strength, device=0) as effect:
            effect.output_width = args.output_width
            effect.output_height = args.output_height
            effect.load()
            report["load_ms"] = (time.perf_counter() - started) * 1000
            report["stage"] = "run"
            times = []
            output = None
            for index in range(args.warmup + args.frames):
                torch.cuda.synchronize()
                started = time.perf_counter()
                result = effect.run(source, non_blocking=False, stream_ptr=0)
                # SDK owns the capsule's buffer. Complete this copy before reuse.
                output = torch.from_dlpack(result.image).clone()
                torch.cuda.synchronize()
                elapsed = (time.perf_counter() - started) * 1000
                if index >= args.warmup:
                    times.append(elapsed)
            report["device_free_after_run"] = torch.cuda.mem_get_info()[0]
            report["stage"] = "validate_output"
            if tuple(output.shape) != (3, args.output_height, args.output_width):
                raise AssertionError(f"Unexpected output shape: {tuple(output.shape)}")
            if not torch.isfinite(output).all().item() or output.std().item() < 0.01:
                raise AssertionError("Output is nonfinite or unexpectedly flat/black")
            baseline = functional.interpolate(source.unsqueeze(0), size=(args.output_height,args.output_width),
                mode="bicubic", align_corners=False).squeeze(0).clamp(0, 1)
            report.update({"output_shape": list(output.shape), "output_range": [output.min().item(),output.max().item()],
                "output_std": output.std().item(), "mse_against_torch_bicubic": (output-baseline).square().mean().item(),
                "samples_ms": times, "median_ms": statistics.median(times),
                "p95_ms": sorted(times)[int(np.ceil(len(times)*0.95))-1],
                "scope": "Synchronized SDK call plus owned GPU output clone; excludes input upload, GL interop, KWin and presentation",
                "async_native_backend": False})
            if args.reference:
                pixels = np.asarray(Image.open(args.reference).convert("RGB")).copy()
                reference = torch.from_numpy(pixels).to("cuda").permute(2, 0, 1).contiguous().float() / 255
                if reference.shape != output.shape:
                    raise ValueError("Reference must match the output dimensions exactly")
                def psnr(image):
                    mse = (image.clamp(0, 1) - reference).square().mean().item()
                    return -10 * math.log10(max(mse, 1e-12))
                report["reference_psnr_rgb_db"] = {"vfx": psnr(output), "torch_bicubic": psnr(baseline)}
                report["quality_scope"] = "Full-frame RGB PSNR on one repeated still; not perceptual or temporal quality"
            if args.image_output:
                args.image_output.parent.mkdir(parents=True, exist_ok=True)
                Image.fromarray(output.clamp(0,1).mul(255).round().byte().permute(1,2,0).cpu().numpy()).save(args.image_output)
            if args.baseline_output:
                args.baseline_output.parent.mkdir(parents=True, exist_ok=True)
                Image.fromarray(baseline.mul(255).round().byte().permute(1,2,0).cpu().numpy()).save(args.baseline_output)
        report["passed"] = True
        report["stage"] = "complete"
    except Exception as error:
        report["error"] = str(error)
        report["exception"] = type(error).__name__
        report["traceback"] = traceback.format_exc()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k:v for k,v in report.items() if k not in ("samples_ms","traceback")}), flush=True)
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
