#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Create a checksummed makepkg recipe from the current source tree, including uncommitted work."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import tarfile

ROOT = Path(__file__).resolve().parents[1]


def source_files(root):
    files=[root/"CMakeLists.txt",root/"README.md"]
    for directory in ("src","scripts","tests","LICENSES","packaging"):
        files.extend(path for path in (root/directory).rglob("*")
                     if path.is_file() and not path.is_symlink() and "__pycache__" not in path.parts and path.suffix!=".pyc")
    files.extend((root/"docs").glob("*.md"))
    return sorted(files,key=lambda path:str(path.relative_to(root)))


def prepare(root, output, nvidia):
    version=re.search(r"project\(kwin_vfx VERSION ([0-9.]+)", (root/"CMakeLists.txt").read_text()).group(1)
    output.mkdir(parents=True,exist_ok=True)
    archive=output/f"kwin-vfx-{version}.tar.gz"
    files=source_files(root)
    epoch=int(os.environ.get("SOURCE_DATE_EPOCH","0"))
    with archive.open("wb") as raw, gzip.GzipFile(fileobj=raw,mode="wb",filename="",mtime=0) as compressed:
        with tarfile.open(fileobj=compressed,mode="w") as tar:
            for path in files:
                info=tar.gettarinfo(str(path),arcname=f"kwin-vfx-{version}/"+str(path.relative_to(root)))
                if not info.isfile():raise ValueError(f"Unexpected source type: {path}")
                info.uid=info.gid=0;info.uname=info.gname="root";info.mtime=epoch
                info.mode=0o755 if info.mode&0o111 else 0o644
                with path.open("rb") as file:tar.addfile(info,file)
    digest=hashlib.sha256(archive.read_bytes()).hexdigest()
    recipe=(root/"packaging/arch/PKGBUILD.in").read_text()
    replacements={"@PACKAGE_SUFFIX@":"-nvidia" if nvidia else "", "@NVIDIA_DEPENDENCY@":" 'nvidia-utils'" if nvidia else "",
        "@CUDA_DEPENDENCY@":" 'cuda'" if nvidia else "", "@NVIDIA_OPTION@":"ON" if nvidia else "OFF",
        "@PACKAGE_RELATIONS@":"provides=('kwin-vfx')\nconflicts=('kwin-vfx' 'kwin-vsr' 'kwin-vsr-nvidia')" if nvidia
            else "conflicts=('kwin-vfx-nvidia' 'kwin-vsr' 'kwin-vsr-nvidia')",
        "@SOURCE_SHA256@":digest}
    for key,value in replacements.items():recipe=recipe.replace(key,value)
    recipe=recipe.replace("pkgver=0.0.1",f"pkgver={version}")
    (output/"PKGBUILD").write_text(recipe)
    result={"package":"kwin-vfx-nvidia" if nvidia else "kwin-vfx","version":version,"source_sha256":digest,
        "source_files":len(files),"proprietary_runtime_bundled":False,"directory":str(output.resolve())}
    (output/"source-manifest.json").write_text(json.dumps(result,indent=2)+"\n")
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-only",action="store_true")
    parser.add_argument("--output-dir",type=Path)
    args=parser.parse_args()
    destination=args.output_dir or ROOT/"build-arch"/("shader" if args.shader_only else "nvidia")
    print(json.dumps(prepare(ROOT,destination,not args.shader_only),indent=2))


if __name__=="__main__":main()
