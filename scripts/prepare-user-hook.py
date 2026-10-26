#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare an opt-in pacman hook that rebuilds a user installation without nested pacman calls."""
import argparse
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def quote(argument):
    value=str(argument)
    if any(character in value for character in "\n\r\0"):
        raise ValueError("Hook arguments cannot contain newlines or NUL bytes")
    return '"'+value.replace("\\","\\\\").replace('"','\\"')+'"'


def hook_text(root, user, prefix, build_dir, cmake_prefixes=(), shader_only=False, cuda_root=None):
    if user.pw_uid==0:raise ValueError("The hook must build as a normal user")
    command=["/usr/bin/runuser","-u",user.pw_name,"--","/usr/bin/env","-i",
             "HOME="+user.pw_dir,"USER="+user.pw_name,"LOGNAME="+user.pw_name,"PATH=/usr/bin:/bin"]
    if cmake_prefixes:command.append("CMAKE_PREFIX_PATH="+os.pathsep.join(map(str,cmake_prefixes)))
    if cuda_root and not shader_only:command.append("CUDAToolkit_ROOT="+str(cuda_root))
    command.extend(["/usr/bin/flock","--nonblock",str(Path(build_dir)/"rebuild.lock"),
                    "/usr/bin/bash",str(Path(root)/"scripts/install-user.sh"),"--no-setup",
                    "--prefix",str(prefix),"--build-dir",str(build_dir)])
    if shader_only:command.append("--shader-only")
    template=(Path(root)/"packaging/arch/kwin-vfx-user.hook.in").read_text()
    return template.replace("@COMMAND@"," ".join(map(quote,command)))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix",type=Path,default=Path.home()/".local")
    parser.add_argument("--build-dir",type=Path,default=ROOT/"build-user-hook")
    parser.add_argument("--output-dir",type=Path,default=ROOT/"build-hook")
    parser.add_argument("--cmake-prefix",type=Path,action="append",default=[])
    nvcc=shutil.which("nvcc")
    cuda_default=os.environ.get("CUDAToolkit_ROOT") or (str(Path(nvcc).resolve().parents[1]) if nvcc else None)
    parser.add_argument("--cuda-root",type=Path,default=Path(cuda_default) if cuda_default else None,
                        help="CUDA toolkit location (detected from CUDAToolkit_ROOT or nvcc when available)")
    parser.add_argument("--shader-only",action="store_true")
    args=parser.parse_args()
    try:
        user=pwd.getpwuid(os.getuid())
        if user.pw_uid==0:raise ValueError("Generate the hook as your desktop user, without sudo")
        packages=subprocess.run(["pacman","-Qq","kwin-vfx","kwin-vfx-nvidia"],capture_output=True,text=True)
        if packages.stdout.strip():
            raise ValueError("A pacman-managed KWin VFX package is installed. Migrate to a user installation first; "
                             "its exact KWin dependency would block upgrades before this hook runs.")
        build=args.build_dir.resolve();prefix=args.prefix.resolve();output=args.output_dir.resolve()
        text=hook_text(ROOT,user,prefix,build,[path.resolve() for path in args.cmake_prefix],args.shader_only,
                       args.cuda_root.resolve() if args.cuda_root else None)
        build.mkdir(parents=True,exist_ok=True);output.mkdir(parents=True,exist_ok=True)
        hook=output/"kwin-vfx.hook";hook.write_text(text)
        print(json.dumps({"hook":str(hook),"user":user.pw_name,"prefix":str(prefix),
                          "build_dir":str(build),"installed":False},indent=2))
        print("Review the generated hook, then install it with:")
        print("sudo install -Dm644 "+quote(hook)+" /etc/pacman.d/hooks/kwin-vfx.hook")
    except (OSError,ValueError) as error:parser.exit(1,f"{error}\n")


if __name__=="__main__":main()
