# Desktop controls

Install using the [packaging guide](packaging.md), then open **KWin VFX** from
the application launcher or run `kwin-vfx-tray --desktop`.

## Selecting windows

Check an application window in **Windows — check to enhance** and uncheck it
to restore normal rendering. Selection uses KWin's UUID and does not change
focus. Several windows from the same application can be enhanced independently.
Closed windows disappear; new windows start unchecked. Normal windows, dialogs
and utility windows are eligible. Panels, desktop backgrounds, popup menus and
lock-screen surfaces are excluded.

**Meta+Ctrl+E** toggles the focused window. **Meta+Ctrl+PageUp/PageDown** adjusts
strength. Shortcuts can be rebound through Plasma's shortcut settings.

Mode, CAS, film grain and strength apply to all checked windows. **Pause all
enhancements** retains selections; **Resume checked windows** resumes them.
**Disable all windows** clears selection. Selecting another window while paused
resumes processing. The unload/enable actions unload or load the effect for the
session. Quitting the tray clears selections first.

## HDR and SDR

**TrueHDR — SDR to HDR** is available only when an active output has HDR enabled.
A disabled HDR-capable display does not enable the control. Output changes are
tracked without a manual effect reload. With HDR output selected, windows on
SDR outputs skip that stage unless the SDR-look option is also enabled.

**TrueHDR look — SDR tone mapping** runs the SDK filter and lets KWin map its
output to SDR. This changes contrast/color on SDR screens; it does not add HDR
display capability. Both options require the NVIDIA runtime/backend.

A window's main output selects the pipeline. Native HDR source windows bypass
SDR enhancement. Mixed-output visual validation remains limited. Filter order
is CAS → optional Deblur/Denoise → TrueHDR → grain → display encoding.
Grain covers the entire selected window and may appear stronger through the
HDR-derived path; adjust its strength to suit the content.

## Session and configuration

Setup enables future loading and tray autostart without restarting KWin.
Window selections are session-only. Preferences use `Effect-kwin_vfx` in
`kwinrc`; the native plugin ID is `kwin_vfx`.

To unload and disable future loading and the managed tray autostart entry:

```sh
kwin-vfx-setup disable
```

After binary upgrades, use a fresh login because Qt may retain the old plugin
library after effect unloading. Ordinary window disable/resize defers worker
cleanup; explicit effect unloading waits for workers to stop safely.

## D-Bus interface

The effect exports `/WindowEnhancement` on the session service `org.kde.KWin`,
interface `org.kde.KWin.WindowEnhancement`:

| Method | Behavior |
| --- | --- |
| `windowList() → string` | JSON window IDs, titles, app classes, selection/focus and HDR availability |
| `setWindowEnabled(string id, bool enabled) → bool` | Explicit selection; rejects closed, invalid or ineligible windows without focusing them |
| `disableAllWindows()` | Clears every selection |

The tray uses the desktop bus with `--desktop`. For isolated testing,
`--preview-session /path/to/session.json` targets a private compositor while
presenting its tray on the parent desktop. Controls unregister on unload.
