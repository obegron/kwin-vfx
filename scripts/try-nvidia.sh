#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export KWIN_VFX_BUILD_DIR="${KWIN_VFX_BUILD_DIR:-$project_root/build-ai}"
export KWIN_VFX_NVIDIA_ROOT="${KWIN_VFX_NVIDIA_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/kwin-vfx/runtime/nvidia-vfx-0.2.0.0}"
export __NGX_LOG_LEVEL=0
case "${KWIN_VFX_NVIDIA_MODE:-8}" in
    8|9|10|11) preview_family='AI denoise' ;;
    12|13|14|15) preview_family='AI deblur' ;;
    *) printf 'The nested preview requires SDK mode 8–15.\n' >&2; exit 1 ;;
esac
if [[ ${1:-} == --video ]]; then
    [[ $# -ge 2 ]] || { printf 'Usage: %s --video /path/to/clip\n' "$0" >&2; exit 1; }
    python3 "$project_root/scripts/prepare-preview.py" "$2" --output-dir "$KWIN_VFX_BUILD_DIR"
    shift 2
fi
if [[ ! -f "$KWIN_VFX_NVIDIA_ROOT/nvidia_vfx-0.2.0.0.dist-info/METADATA" ]]; then
    printf 'NVIDIA runtime missing at %s. See docs/validation.md.\n' "$KWIN_VFX_NVIDIA_ROOT" >&2
    exit 1
fi
if [[ ! -f "$KWIN_VFX_BUILD_DIR/plugins/kwin/effects/plugins/kwin_vfx.so" ]]; then
    printf 'Build with KWIN_VFX_BUILD_NVIDIA_BACKEND=ON first. See docs/validation.md.\n' >&2
    exit 1
fi
printf 'NVIDIA %s preview: focus a window inside nested KWin and press Meta+Ctrl+E.\n' "$preview_family"
printf 'Strength: Meta+Ctrl+PageUp/PageDown. Close the nested compositor to exit.\n'
if [[ -f "$KWIN_VFX_BUILD_DIR/preview.html" ]]; then
    export KWIN_VFX_PREVIEW_URL="file://$KWIN_VFX_BUILD_DIR/preview.html"
fi
exec "$project_root/scripts/run-nested.sh" "$@"
