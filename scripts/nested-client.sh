#!/usr/bin/env bash
set -euo pipefail
[[ ${KWIN_VFX_NESTED:-} == 1 && -n ${KWIN_VFX_RUN_DIR:-} ]] || exit 1
[[ ${1:-} != -- ]] || shift
unset DISPLAY
export QT_QPA_PLATFORM=wayland MOZ_ENABLE_WAYLAND=1
python3 -c 'import json,os,pathlib; p=pathlib.Path(os.environ["KWIN_VFX_RUN_DIR"])/"session.json"; p.write_text(json.dumps({key:os.environ.get(key,"") for key in ("DBUS_SESSION_BUS_ADDRESS","WAYLAND_DISPLAY","XDG_RUNTIME_DIR","KWIN_VFX_RUN_DIR","KWIN_VFX_BUILD_DIR")},indent=2)+"\n"); p.chmod(0o600)'
if [[ $(qdbus6 org.kde.KWin /Effects org.kde.kwin.Effects.loadEffect kwin_vfx) != true ]]; then
    printf 'Failed to load kwin_vfx in the nested compositor\n' >&2
    qdbus6 org.kde.KWin /Effects
    exit 1
fi
exec "$@"
