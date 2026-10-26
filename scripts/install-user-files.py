#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage a CMake user installation and replace files without truncating mapped libraries."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def install(build_dir, prefix):
    build_dir=Path(build_dir).resolve()
    prefix=Path(prefix).resolve()
    with tempfile.TemporaryDirectory(prefix=".kwin-vfx-install-",dir=build_dir) as directory:
        subprocess.run(["cmake","--install",str(build_dir)],
                       env=dict(os.environ,DESTDIR=directory),check=True)
        payload=Path(directory)/prefix.relative_to(prefix.anchor)
        entries=list(payload.rglob("*"))
        if any(path.is_symlink() or not (path.is_dir() or path.is_file()) for path in entries):
            raise ValueError("Unexpected symlink or special file in the staged installation")
        sources=sorted(path for path in entries if path.is_file())
        if not sources:raise ValueError("CMake did not stage any files under the installation prefix")
        for source in sources:
            destination=prefix/source.relative_to(payload)
            destination.parent.mkdir(parents=True,exist_ok=True)
            with tempfile.NamedTemporaryFile(prefix=".kwin-vfx-",dir=destination.parent,delete=False) as file:
                temporary=Path(file.name)
            try:
                shutil.copy2(source,temporary)
                temporary.replace(destination)
            finally:
                temporary.unlink(missing_ok=True)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path,required=True)
    parser.add_argument("--prefix",type=Path,required=True)
    args=parser.parse_args()
    if os.geteuid()==0:parser.error("Run the user installer without sudo")
    try:install(args.build_dir,args.prefix)
    except (OSError,ValueError,subprocess.CalledProcessError) as error:parser.exit(1,f"{error}\n")


if __name__=="__main__":main()
