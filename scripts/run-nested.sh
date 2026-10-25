#!/usr/bin/env bash
set -euo pipefail

# All D-Bus effect operations belong inside the new session below.
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=$(realpath "${KWIN_VFX_BUILD_DIR:-$project_root/build}")
if [[ ! -f "$build_dir/plugins/kwin/effects/plugins/kwin_vfx.so" ]]; then
    printf 'Build the effect first: %s\n' "$build_dir" >&2
    exit 1
fi
if [[ ${KWIN_VFX_TEST_HEADLESS:-0} != 1 && -z ${WAYLAND_DISPLAY:-} ]]; then
    printf 'A parent Wayland session is required.\n' >&2
    exit 1
fi
run_dir=$(mktemp -d /tmp/kwin-vfx-nested.XXXXXX)
mkdir -p "$run_dir/config" "$run_dir/data" "$run_dir/cache"
cat > "$run_dir/config/kwinrc" <<'EOF'
[Plugins]
kwin_vfxEnabled=false
wobblywindowsEnabled=false
fadeEnabled=false
fadingpopupsEnabled=false
slideEnabled=false
EOF
printf 'Nested-session artifacts: %s\n' "$run_dir"
export KWIN_VFX_NESTED=1 KWIN_VFX_RUN_DIR="$run_dir" KWIN_VFX_BUILD_DIR="$build_dir"
export XDG_CONFIG_HOME="$run_dir/config" XDG_DATA_HOME="$run_dir/data" XDG_CACHE_HOME="$run_dir/cache"
export QT_PLUGIN_PATH="$build_dir/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export QT_LOGGING_RULES="${QT_LOGGING_RULES:+$QT_LOGGING_RULES;}kwin.effect.vfx.info=true"
export QT_FORCE_STDERR_LOGGING=1
if [[ $# == 0 ]]; then
    set -- firefox --no-remote --profile "$run_dir/firefox-profile" "${KWIN_VFX_PREVIEW_URL:-about:blank}"
    mkdir -p "$run_dir/firefox-profile"
    cat > "$run_dir/firefox-profile/user.js" <<'EOF'
user_pref("media.autoplay.default", 0);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.startup.homepage_override.mstone", "ignore");
EOF
fi
{
    printf '#!/usr/bin/env bash\nexec '
    printf '%q ' "$project_root/scripts/nested-client.sh" "$@"
    printf '\n'
} > "$run_dir/client.sh"
chmod +x "$run_dir/client.sh"
# This disposable compositor must not start a second lock screen when logind
# reports the desktop idle/locked. The parent desktop retains its own lock.
backend_args=(--wayland-display "${WAYLAND_DISPLAY:-}")
if [[ ${KWIN_VFX_TEST_HEADLESS:-0} == 1 ]]; then
    backend_args=(--virtual)
    export KWIN_VFX_TEST_OUTPUT=Virtual-0
fi
dbus-run-session -- kwin_wayland --no-lockscreen "${backend_args[@]}" \
    --socket "kwin-vfx-$(basename "$run_dir")" \
    --width "${KWIN_VFX_TEST_WIDTH:-1920}" --height "${KWIN_VFX_TEST_HEIGHT:-1080}" \
    --scale "${KWIN_VFX_TEST_SCALE:-1}" \
    --exit-with-session "$run_dir/client.sh" \
    > "$run_dir/kwin.log" 2>&1
