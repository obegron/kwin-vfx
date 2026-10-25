#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
if ((EUID == 0)); then printf 'Run the user installer as your desktop user, without sudo.\n' >&2; exit 1; fi
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
install_prefix="${HOME}/.local"
build_dir="${project_root}/build-user"
nvidia=ON
wheel_file=''
download_runtime=0
runtime_root=''
stage_dir=''
setup=1
while (($#)); do
    case "$1" in
        --shader-only) nvidia=OFF; shift ;;
        --prefix) install_prefix="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --wheel) wheel_file="$2"; shift 2 ;;
        --download-runtime) download_runtime=1; shift ;;
        --runtime-root) runtime_root="$2"; shift 2 ;;
        --stage) stage_dir="$2"; setup=0; shift 2 ;;
        --no-setup) setup=0; shift ;;
        -h|--help)
            printf 'Usage: scripts/install-user.sh [--shader-only] [--wheel FILE | --download-runtime] [--runtime-root DIR]\n'
            printf 'Builds from source and installs under ~/.local without sudo. First use needs a logout/login.\n'
            printf 'Options: --prefix DIR --build-dir DIR --no-setup --stage DIR (testing, no live settings)\n'
            exit 0 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done
if [[ -n "$wheel_file" && "$download_runtime" == 1 ]]; then
    printf 'Choose --wheel or --download-runtime.\n' >&2; exit 2
fi
if [[ "$nvidia" == OFF && ( -n "$wheel_file" || "$download_runtime" == 1 ) ]]; then
    printf 'Runtime options require the NVIDIA build.\n' >&2; exit 2
fi
cmake -S "$project_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX="$install_prefix" \
    -DKDE_INSTALL_PLUGINDIR=lib/kwin-vfx/plugins \
    -DKWIN_VFX_BUILD_NVIDIA_BACKEND="$nvidia" -DKWIN_VFX_BUILD_PREVIEW_TRAY=ON
cmake --build "$build_dir" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
if [[ -n "$stage_dir" ]]; then
    DESTDIR="$stage_dir" cmake --install "$build_dir"
    printf 'Staged user installation in %s; desktop settings were not changed.\n' "$stage_dir"
    exit 0
fi
cmake --install "$build_dir"
runtime_args=()
if [[ -z "$runtime_root" && ( -n "$wheel_file" || "$download_runtime" == 1 ) ]]; then
    runtime_root="${XDG_DATA_HOME:-$HOME/.local/share}/kwin-vfx/runtime/nvidia-vfx-0.2.0.0"
fi
if [[ -n "$runtime_root" ]]; then runtime_args=(--output-dir "$runtime_root"); fi
if [[ -n "$wheel_file" ]]; then "$install_prefix/bin/kwin-vfx-runtime" "$wheel_file" "${runtime_args[@]}"; fi
if [[ "$download_runtime" == 1 ]]; then "$install_prefix/bin/kwin-vfx-runtime" --download "${runtime_args[@]}"; fi
if [[ "$setup" == 1 ]]; then
    setup_args=(enable --user-plugin-path)
    if [[ -n "$runtime_root" ]]; then setup_args+=(--runtime-root "$runtime_root"); fi
    "$install_prefix/bin/kwin-vfx-setup" "${setup_args[@]}"
fi
