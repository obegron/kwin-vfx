#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
variant=nvidia
prepare_args=()
if [[ ${1:-} == --shader-only ]]; then
    variant=shader
    prepare_args=(--shader-only)
    shift
fi
if [[ ${1:-} == --help ]]; then
    printf 'Usage: scripts/build-arch.sh [--shader-only] [makepkg arguments]\n'
    printf 'Example: scripts/build-arch.sh -si (build from source, install dependencies and package)\n'
    exit 0
fi
python3 "$project_root/scripts/prepare-arch-package.py" "${prepare_args[@]}"
cd "$project_root/build-arch/$variant"
exec makepkg "$@"
