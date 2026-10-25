# Validation

Run compositor tests in a disposable nested KWin session first. The launcher
uses temporary configuration/data directories, a private D-Bus session and a
separate Firefox profile. Closing it exits the test compositor. Logs and
screenshots stay in the printed temporary directory; they can contain window
titles and local paths and should be reviewed before sharing.

## Build

Install the development dependencies in [packaging](packaging.md), then:

```sh
cmake -S . -B build -DKWIN_VFX_BUILD_SMOKE_CLIENT=ON -DKWIN_VFX_BUILD_SHADER_TESTS=ON -DKWIN_VFX_CHECK_HEADERS=ON
cmake --build build
python3 tests/packaging_test.py
build/kwin_vfx_profile_reset_test
scripts/run-nested.sh python tests/enhancement-smoke.py
scripts/run-nested.sh python tests/cas-smoke.py
scripts/run-nested.sh python tests/grain-smoke.py
scripts/run-nested.sh python tests/window-control-smoke.py
```

Python compositor tests require `python-dbus` and `python-pillow`. Browser tests
also require Firefox and FFmpeg. `KWIN_VFX_BUILD_DIR` selects a different build.
`KWIN_VFX_TEST_WIDTH`, `KWIN_VFX_TEST_HEIGHT` and `KWIN_VFX_TEST_SCALE` control
the nested output; dimensions are logical and a nested Wayland output may follow
the parent's preferred scale. Record the measured screenshot/output scale.

To check the real tray menu against the private effect, preserve the parent
bus/display for the tray only:

```sh
KWIN_VFX_TEST_TRAY_BUS="$DBUS_SESSION_BUS_ADDRESS" KWIN_VFX_TEST_TRAY_DISPLAY="$WAYLAND_DISPLAY" scripts/run-nested.sh python tests/window-tray-smoke.py
```

This checks window selection, focus preservation, menu synchronization,
pause/resume, closed-window removal and HDR availability. Tests in
`nested_support.py` reject execution outside the nested launcher. The explicit
`desktop-smoke.py --desktop` test instead creates two fixture windows in the
live session; use it only after private-session validation.

## Repeatable browser video

Generate fixed, non-DRM 720p and 1080p moving patterns:

```sh
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 12 -c:v libvpx-vp9 -b:v 1M -y build/720p.webm
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30 -t 12 -c:v libvpx-vp9 -b:v 2M -y build/1080p.webm
scripts/run-nested.sh python tests/browser-smoke.py
```

The browser test serves only on loopback, uses an isolated Firefox profile and
Marionette connection, and records playback counters and effect state. A local
clip can be supplied with `KWIN_VFX_BROWSER_CLIP=/path/to/video.mp4`. For manual
comparison, open Firefox in the nested session with `scripts/run-nested.sh` and
load a fixed non-DRM source. Check toggle behavior, browser text readability,
window movement, resizing, fullscreen, overlapping windows and smooth playback.
Repeat at 1080p and 4K output, including fractional scaling.

Synthetic patterns establish rendering behavior, not subjective video quality.
For natural content, use a fixed clip such as the
[Blender Sintel trailer](https://download.blender.org/durian/trailer/sintel_trailer-1080p.mp4),
record its checksum and keep the same paused frame for visual comparisons.
Screenshots must not be treated as an objective quality score.

## NVIDIA backend

Install the supported runtime separately with `kwin-vfx-runtime --download`,
or use `python3 scripts/prepare-nvidia-runtime.py --download`. Then build:

```sh
cmake -S . -B build-ai -DKWIN_VFX_BUILD_NVIDIA_BACKEND=ON -DKWIN_VFX_BUILD_PREVIEW_TRAY=ON -DKWIN_VFX_BUILD_NVIDIA_PROBES=ON -DKWIN_VFX_BUILD_SMOKE_CLIENT=ON -DKWIN_VFX_BUILD_SHADER_TESTS=ON
cmake --build build-ai
export KWIN_VFX_NVIDIA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}/kwin-vfx/runtime/nvidia-vfx-0.2.0.0"
scripts/try-nvidia.sh python tests/nvidia-smoke.py
scripts/try-nvidia.sh python tests/retirement-smoke.py
scripts/try-nvidia.sh --video /path/to/video.mp4
```

The preview defaults to AI denoise. Set `KWIN_VFX_NVIDIA_MODE=12` for deblur.
The optional tray can target the printed `session.json` via
`build-ai/kwin_vfx_preview_tray --preview-session /path/to/session.json`.
SDK failures should return to shader/original drawing. Also test missing runtime,
unsupported dimensions, mode changes, repeated disable/enable, interactive
resize, window closure and explicit effect unload/reload.

Standalone probes isolate SDK and interop failures from KWin:
`kwin_vfx_cuda_gl_probe`, `kwin_vfx_native_vfx_probe` and
`kwin_vfx_worker_probe`. The native SDK probe accepts `--help`. Run the worker
probe with `--runtime-root "$KWIN_VFX_NVIDIA_ROOT" --mode 8 --frames 24`; mode 0
checks identity transfer, and `--true-hdr` adds the HDR stage. The worker probe
covers the shared EGL context and fence topology; a passing single-context
interop probe alone does not validate that path. Python SDK probes require the
wheel's Python dependencies and are independent of the native effect.

## Timing and limits

```sh
QT_QPA_PLATFORM=wayland build/kwin_vfx_shader_probe --benchmark --output build/shader-results.json
scripts/run-nested.sh python tests/profile-smoke.py
```

Report CPU submission time separately from GPU shader query time, worker
inference/completion time, and end-to-end presentation latency. The effect's
`debug()` JSON includes capture/output revision, pipeline state and SDK/worker
timing. A revision gap greater than one rejects stale AI output, so throughput
must be considered alongside the fraction of frames that actually use AI.
Compare timing with the display budget (16.7 ms at 60 Hz), recording dimensions,
scale, refresh, selected-window count, GPU load and video playback counters.

Private-session checks cover rendering and lifecycle behavior, but do not
certify sustained 4K frame deadlines, mixed HDR/SDR output appearance, arbitrary
effect ordering or driver recovery. Test actual HDR and SDR presentation
separately; encoded PQ metadata alone is not proof of correct display output.
