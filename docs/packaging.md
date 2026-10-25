# Packaging KWin VFX

The native plugin ID is `kwin_vfx`,
the settings group is `Effect-kwin_vfx`, and commands are `kwin-vfx-tray`,
`kwin-vfx-setup` and `kwin-vfx-runtime`. The repository is
[obegron/kwin-vfx](https://github.com/obegron/kwin-vfx).

The plugin and tray are built from our source. NVIDIA's runtime stays separate:
the runtime helper obtains a pinned official wheel directly from NVIDIA or uses
a local copy, verifies its SHA256 and extracts its native libraries into the
user's data directory. It does not run pip or install Torch/CuPy. KWin remains
entirely native C++; Python is used only for installation/setup helpers.

## Arch: build a package with makepkg

From this checkout:

```sh
scripts/build-arch.sh -si
kwin-vfx-runtime --download
kwin-vfx-setup enable
kwin-vfx-tray --desktop
```

The first command prepares a source archive from the current working tree,
generates a SHA256-pinned **PKGBUILD**, and
runs `makepkg -si`. It builds **kwin-vfx-nvidia**. CUDA is a build dependency;
the installed package requires the NVIDIA driver libraries, not the full CUDA
toolkit. The proprietary wheel is not included in source or binary packages.
The package installs the plugin, tray, launcher, setup/runtime commands and
license notices. Installation does not download a wheel, change user settings,
start a compositor or enable a login service. Those are per-user setup steps.

For a CUDA/NVIDIA-independent CAS/grain/shader package:

```sh
scripts/build-arch.sh --shader-only -si
kwin-vfx-setup enable
```

This builds **kwin-vfx**, which conflicts with the NVIDIA variant. Both variants
are pinned to the audited **KWin 6.8.1** private API. A KWin update requires a
matching source update/rebuild; source building alone does not guarantee that
changed private APIs remain compatible. Package version constraints make that
dependency explicit. GPU/compositor tests are separate from `makepkg check()`;
the package's checks cover the CPU-only profiler and offline installer/archive
regressions, and work without a desktop or CUDA device.

To inspect the recipe before building:

```sh
python3 scripts/prepare-arch-package.py
cd build-arch/nvidia
less PKGBUILD
makepkg --verifysource
makepkg -si
```

The helper generates a local-source recipe. After building,
`makepkg --allsource` can create a source package for sharing. [Arch's makepkg guide](https://wiki.archlinux.org/title/Makepkg)
and [PKGBUILD reference](https://wiki.archlinux.org/title/PKGBUILD) describe this workflow.

## User-only source installation

With development dependencies installed, this needs no sudo:

```sh
scripts/install-user.sh --download-runtime
```

For an already downloaded wheel or a shader-only installation:

```sh
scripts/install-user.sh --wheel /path/to/official-nvidia-vfx.whl
scripts/install-user.sh --shader-only
```

The installer builds locally and installs commands under `~/.local/bin`, the
plugin under `~/.local/lib/kwin-vfx/plugins/kwin/effects/plugins`, and desktop
assets under `~/.local/share`. The default native runtime goes under
`${XDG_DATA_HOME:-~/.local/share}/kwin-vfx/runtime/nvidia-vfx-0.2.0.0`.
The download cache is `${XDG_CACHE_HOME:-~/.cache}/kwin-vfx`.
Use the full `~/.local/bin/kwin-vfx-*` command paths if that directory is not
already in your shell's PATH.

**Log out and back in once after the first user installation.** Native KWin
plugins do not gain a user search path merely by being copied to `~/.local`.
Setup creates `~/.config/systemd/user/plasma-kwin_wayland.service.d/60-kwin-vfx.conf`
with a plugin-path setting scoped to KWin. It reloads systemd's unit definitions;
it never restarts KWin. Qt needs that environment when KWin starts. This route
targets Plasma's standard systemd user service; a custom non-systemd session
needs the same plugin path in its compositor launcher. See
[Qt's plugin discovery documentation](https://doc.qt.io/qt-6/deployment-plugins.html)
and [systemd's Environment directive](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html#Environment=).

Arch dependencies for the shader build are `base-devel cmake ninja
extra-cmake-modules kwin qt6-base qt6-declarative kglobalaccel kdecoration
vulkan-headers python python-dbus`; the NVIDIA build adds `cuda` and compatible
`nvidia-utils`. KWin's development CMake exports bring in its other development
dependencies. CMake rejects an un-audited KWin version. The installer does not
silently install system dependencies; `makepkg -si` is the dependency-managed route.

`--prefix` and `--build-dir` customize a source install. `--no-setup` installs files
without changing desktop settings. `--stage DIR` installs into a test staging
root and skips runtime downloads and live setup. Inspect configuration changes
without applying them using `kwin-vfx-setup enable --user-plugin-path --dry-run`.

## NVIDIA runtime and distribution

```sh
kwin-vfx-runtime --download
kwin-vfx-runtime /path/to/official-wheel.whl
kwin-vfx-runtime --status
kwin-vfx-setup enable
```

The supported wheel is **nvidia-vfx 0.2.0.0**, SHA256
`0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8`.
Native libraries and their complete bundled license files are kept together.
Runtime installation publishes a new version directory atomically and refuses
to overwrite an incomplete/different/unverified directory or mutate libraries a worker
might have mapped. A verified existing runtime is reused. `--output-dir` selects
another runtime location; pass that directory to `kwin-vfx-setup enable --runtime-root`.

The wheel metadata marks it `LicenseRef-NvidiaProprietary`; its bundled May 2025
software agreement, product-specific terms and third-party notices govern its
components. This packaging deliberately does not redistribute the wheel or
assert that all its components have identical redistribution permissions.
[NVIDIA's agreement page](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-license-agreement/)
may show newer terms; keep/read the licenses bundled with the pinned wheel too.
The GPL source and AMD CAS MIT notice remain in the source/package.
Building from source produces a normal binary `.pkg.tar.zst`; the reason to
rebuild locally is KWin API compatibility, not a technical inability to package
our compiled plugin. Arch/GPU support remains the tested project scope rather
than a claim of NVIDIA support for Arch.

## Migrating the old name and updating

Setup copies existing `Effect-kwin_vsr` preferences into the new group, preserving
values already set under `Effect-kwin_vfx`. It keeps the existing runtime path
when no newer verified per-user runtime is installed, disables old plugin autoload and backs up the old tray autostart
entry. It does not rename the checkout directory or move a live runtime.

If the old plugin is running, setup leaves it running and requests a fresh login
to switch names. It does not load both plugins against the same controls. The
old manual system plugin may remain on disk, with its autoload disabled. Current
window selections are session-only, so windows start unchecked after login.

Use a fresh login after binary upgrades too: Qt can retain a plugin DSO after
effect unloading. Pacman owns the packaged files for clean upgrades/removal.
For a user build, rebuild/re-run the source installer and log in again; copying
new files does not replace code already mapped by the running compositor.

`kwin-vfx-setup disable` disables autoload, unloads the new effect, and removes its
managed tray autostart entry. To remove a user installation completely after
disabling it, remove only its `kwin-vfx-*` commands, `~/.local/lib/kwin-vfx`,
`~/.local/share/kwin-vfx/installation.json`, launcher/license entries, and the
`60-kwin-vfx.conf` service drop-in; keep the runtime/preferences if you want to
reuse them. A different `--prefix` changes those paths.
