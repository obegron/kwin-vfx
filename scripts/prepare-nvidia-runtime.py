#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Install a verified NVIDIA runtime per user, without pip or Python dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import sys
import tempfile
import urllib.request
import zipfile

VERSION = "0.2.0.0"
WHEEL = f"nvidia_vfx-{VERSION}-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl"
URL = "https://pypi.nvidia.com/nvidia-vfx/" + WHEEL
SHA256 = "0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8"
TERMS = "https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-license-agreement/"
PRODUCT_TERMS = "https://www.nvidia.com/en-us/agreements/enterprise-software/product-specific-terms-for-ai-products/"
METADATA = f"nvidia_vfx-{VERSION}.dist-info/METADATA"
REQUIRED = (METADATA, "nvvfx/libs/libNVCVImage.so", "nvvfx/libs/libVideoFX.so",
            "nvvfx/libs/libnvVFXVideoSuperRes.so", "nvvfx/libs/libnvidia-ngx-vsr.so.1.8.3",
            "nvvfx/libs/libnvVFXTrueHDR.so", "nvvfx/libs/libnvidia-ngx-truehdr.so.1.2.3")


def default_runtime():
    return Path(os.environ.get("XDG_DATA_HOME", str(Path.home()/".local/share"))) / "kwin-vfx/runtime" / f"nvidia-vfx-{VERSION}"


def require_license_acceptance(accepted):
    print(f"NVIDIA VFX {VERSION} is proprietary software, licensed separately from KWin VFX.\n"
          f"NVIDIA Software License Agreement: {TERMS}\n"
          f"Product-specific terms: {PRODUCT_TERMS}\n"
          "The wheel includes the May 2025 agreement and product-specific terms,\n"
          "model licenses and third-party notices under its .dist-info/licenses directory.\n"
          "The reference pages above may contain newer terms; review the bundled versions too.\n"
          "Proceed only if you have reviewed and accept the applicable NVIDIA terms.",
          file=sys.stderr, flush=True)
    if accepted:
        return
    if sys.stdin.isatty():
        print("Do you accept the applicable NVIDIA license terms? [y/N] ", end="", file=sys.stderr, flush=True)
        try:
            answer = input().strip().lower()
        except EOFError:
            answer = ""
        if answer in ("y", "yes"):
            return
    raise ValueError("NVIDIA license not accepted; no download or installation was performed. "
                     "For unattended installation, review the terms and pass --accept-license.")


def verify_wheel(path):
    with Path(path).open("rb") as file:
        actual = hashlib.file_digest(file, "sha256").hexdigest()
    if actual != SHA256:
        raise ValueError(f"Wheel SHA256 differs from the audited release: {actual}")


def runtime_status(path):
    path = Path(path)
    missing = [name for name in REQUIRED if not (path/name).is_file()]
    metadata_ok = not missing and f"\nVersion: {VERSION}\n" in (path/METADATA).read_text()
    receipt=path/"kwin-vfx-runtime.json"
    try:
        verified=receipt.is_file() and json.loads(receipt.read_text()).get("sha256")==SHA256
    except (ValueError,OSError):verified=False
    return {"ready": bool(metadata_ok), "version": VERSION, "path": str(path.resolve()), "missing": missing,
            "verified_archive":bool(verified),"licenses": str(path/f"nvidia_vfx-{VERSION}.dist-info/licenses"), "python_dependencies_installed": False}


def install_wheel(wheel, destination):
    verify_wheel(wheel)
    destination = Path(destination).absolute()
    if destination.exists():
        existing=runtime_status(destination)
        if existing["ready"] and existing["verified_archive"]:return existing
        raise ValueError(f"Refusing to overwrite an incomplete/different/unverified runtime: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    # Never overwrite libraries potentially mapped by a worker. Publish a
    # complete new version directory with one atomic rename.
    with tempfile.TemporaryDirectory(prefix=".nvidia-vfx-", dir=destination.parent) as temporary:
        staging = Path(temporary)/"runtime"
        staging.mkdir()
        with zipfile.ZipFile(wheel) as archive:
            for member in archive.infolist():
                name = PurePosixPath(member.filename)
                mode = member.external_attr >> 16
                if name.is_absolute() or ".." in name.parts or "\\" in member.filename or stat.S_ISLNK(mode):
                    raise ValueError(f"Unsafe wheel entry: {member.filename}")
            archive.extractall(staging)
        report = runtime_status(staging)
        if not report["ready"]:
            raise ValueError(f"Wheel does not contain the expected native runtime: {report['missing']}")
        (staging/"kwin-vfx-runtime.json").write_text(json.dumps({"wheel":WHEEL,"sha256":SHA256,"version":VERSION},indent=2)+"\n")
        staging.rename(destination)
    return runtime_status(destination)


def download_wheel(cache):
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    wheel = cache/WHEEL
    if wheel.exists():
        verify_wheel(wheel)
        return wheel
    print(f"Downloading directly from NVIDIA: {URL}\nNVIDIA terms: {TERMS}", file=sys.stderr, flush=True)
    with tempfile.NamedTemporaryFile(prefix=".nvidia-vfx-", dir=cache, delete=False) as file:
        temporary = Path(file.name)
        try:
            with urllib.request.urlopen(URL, timeout=60) as response:
                shutil.copyfileobj(response, file, length=1024*1024)
            file.flush()
            verify_wheel(temporary)
            temporary.rename(wheel)
        finally:
            temporary.unlink(missing_ok=True)
    return wheel


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog=f"NVIDIA's included licenses apply. Terms: {TERMS}")
    parser.add_argument("wheel", nargs="?", type=Path, help="Local official wheel (offline installation)")
    parser.add_argument("--download", action="store_true", help="Fetch the pinned wheel directly from NVIDIA")
    parser.add_argument("--accept-license", action="store_true", help="Confirm you have reviewed and accept the applicable NVIDIA license terms")
    parser.add_argument("--output-dir", type=Path, default=default_runtime())
    parser.add_argument("--cache-dir", type=Path, default=Path(os.environ.get("XDG_CACHE_HOME",str(Path.home()/".cache")))/"kwin-vfx")
    parser.add_argument("--status", action="store_true", help="Inspect the native layout without changing anything")
    args = parser.parse_args()
    try:
        if args.status:
            if args.download or args.wheel: parser.error("--status does not download or install")
            result = runtime_status(args.output_dir)
        else:
            if bool(args.wheel) == args.download: parser.error("Provide a local wheel or --download")
            require_license_acceptance(args.accept_license)
            wheel = download_wheel(args.cache_dir) if args.download else args.wheel
            result = install_wheel(wheel,args.output_dir)
        print(json.dumps(result,indent=2))
        if not result["ready"]: return 1
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1,f"{error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
