# KWin VFX

Native per-window visual enhancement for KDE Plasma 6 on Wayland. Apply
sharpening, denoise, deblur and film grain to application windows, including
browsers and games, without extensions or a separate playback window.

**Experimental; requires KWin 6.8.1 and its OpenGL compositor.** The plugin uses
private KWin APIs and must be reviewed and rebuilt for other KWin versions.

- GPU shaders provide Contrast Adaptive Sharpening (CAS), spatial denoise and
  animated monochrome film grain without NVIDIA dependencies.
- The optional NVIDIA backend provides AI denoise/deblur and TrueHDR using a
  separately installed NVIDIA VFX runtime. It processes the composed window;
  it does not perform source-resolution video super-resolution.
- A system tray menu selects individual windows and adjusts mode, strength,
  CAS, grain and HDR options. Several windows can be selected independently.
- **Meta+Ctrl+E** toggles the focused window. **Meta+Ctrl+PageUp/PageDown** adjusts
  strength. New windows start unchecked.

## Installation

On Arch Linux, build and install the NVIDIA-enabled package from this checkout:

```sh
scripts/build-arch.sh -si
kwin-vfx-runtime --download
kwin-vfx-setup enable
kwin-vfx-tray --desktop
```

For shaders only, use `scripts/build-arch.sh --shader-only -si` and skip the
runtime command. A user installation without sudo is also available:

```sh
scripts/install-user.sh --download-runtime
# Or: scripts/install-user.sh --shader-only
```

Log out and back in after the first user installation so KWin receives its
plugin search path. Setup never restarts the compositor. See the
[packaging guide](docs/packaging.md) for dependencies, runtime licensing,
configuration and removal.

## Use

Open **KWin VFX** from the application launcher and check windows under
**Windows — check to enhance**. Settings apply to every checked window.
Pause retains the selection; disabling all windows clears it.

**TrueHDR — SDR to HDR** requires an active HDR-enabled display.
**TrueHDR look — SDR tone mapping** is a separate option for SDR displays.
Film grain covers the entire selected window, including application UI.
See [desktop controls](docs/desktop.md) for details.

## Development

Build and test inside a separate nested compositor before loading a changed
plugin into the desktop session:

```sh
cmake -S . -B build -DKWIN_VFX_BUILD_SMOKE_CLIENT=ON -DKWIN_VFX_BUILD_SHADER_TESTS=ON
cmake --build build
scripts/run-nested.sh python tests/enhancement-smoke.py
```

The [validation guide](docs/validation.md) covers browser fixtures, NVIDIA setup
and benchmarks. The [architecture](docs/architecture.md) describes capture,
GPU ownership, synchronization and limitations; [src/README.md](src/README.md)
maps the implementation.

AI processing is asynchronous and may miss frame deadlines at large window
sizes. Stale results fall back to shader processing. Native HDR source windows
bypass SDR enhancement. Mixed HDR/SDR output behavior and sustained frame
latency need broader testing; this is an in-process compositor plugin.

Licensed under [GPL-2.0-or-later](LICENSES/GPL-2.0-or-later.txt), with
[AMD CAS code under MIT](LICENSES/MIT-AMD-CAS.txt). NVIDIA libraries and models
are not included.
