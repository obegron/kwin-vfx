# Architecture

KWin VFX is an in-process OpenGL effect for **KWin 6.8.1**. The implementation
was checked against the matching KWin source and installed headers. These are
private interfaces, not a stable third-party ABI. CMake and the Arch package
pin that version; a KWin update requires source review and rebuilding.

## Rendering integration

The relevant upstream interfaces are in the
[KWin 6.8.1 source tree](https://invent.kde.org/plasma/kwin/-/tree/v6.8.1/src):
`effect/effect.h`, `effect/effecthandler.h`, `effect/effectwindow.h`,
`effect/offscreeneffect.cpp`, `scene/windowitem.h` and `opengl/glutils.h`.
`drawWindow` participates in the effect chain with a render target, viewport,
window, region and paint data. A false return signals a rendering failure.

Two capture approaches are possible:

| Approach | Advantages | Limitations |
| --- | --- | --- |
| `OffscreenEffect` plus a fragment shader, as used by KWin Invert | Existing redirection, cached capture and color-aware drawing | No public captured-texture getter or asynchronous handoff; limited control over helper failure paths |
| Effect-owned texture/FBO capture through `drawWindow` | Explicit ownership, failure recovery and CUDA staging | Must preserve clipping, scaling, transforms, alpha, color and scanout restrictions |

The implementation uses effect-owned capture to support multipass processing
and normal-window fallback. It adapts KWin's offscreen drawing patterns with
scoped framebuffer restoration and explicit allocation/capture/map failures.
An `ItemEffect` prevents selected window items from bypassing compositing.
Unselected windows continue through the ordinary draw chain.

Each selected window owns an SDR offscreen texture, damage state and capture
revision. Capture refreshes on damage and relevant geometry/scale changes.
The final shader respects viewport transforms, clipping, opacity, brightness,
saturation and premultiplied alpha. Native HDR source windows bypass this SDR
capture/enhancement pipeline.

KWin sees the application's composed window, including text, decorations and
subsurfaces. Browsers generally scale video before submitting that buffer.
The original decoded video resolution and video-only texture are unavailable
through this path. Whole-window processing therefore cannot promise native
720p-to-4K video super-resolution. Film grain and other filters also affect UI.
Directly retaining a client surface texture would require handling scene-owned
buffer lifetimes and acquire/release synchronization; the effect avoids that.

## Shader processing

The CUDA-independent backend provides spatial denoise/sharpen, AMD CAS and
animated monochrome grain. Shaders are shared with independent correctness and
GPU timing probes. Strength zero permits passthrough without capture work when
no other processing is needed. Shader/allocation failures drop the selection
and restore ordinary rendering where the GL context remains usable.

## NVIDIA ownership and synchronization

The optional native backend loads VFX SDK libraries from the separately
installed, pinned NVIDIA wheel. Same-size SDK modes 8–11 provide denoise and
12–15 provide deblur. These belong to NVIDIA's `VideoSuperRes` API family but
are not spatial upscaling modes. TrueHDR is an optional subsequent SDK stage.
The bindings use documented C entry points and SDK-created opaque descriptors.
No Python interpreter runs inside KWin.

A per-window slot stages a GPU-only input copy, allowing KWin's current capture
to remain independent of an in-flight job. A worker owns a shared EGL context
and CUDA registrations. Only one job is in flight per slot. A GL producer fence
precedes CUDA mapping; worker completion and consumer fences protect output
publication and reuse. Two alternating output textures allow GL to sample the
published result while CUDA writes the other texture. Map/unmap, inference and
completion waits occur on the worker thread. This avoids CPU frame readback,
but still involves GPU copies and format conversions.

A completed result more than one capture revision behind is rejected in favor
of the shader/original path, avoiding blending widely separated motion frames.
This can reduce the visible AI contribution when inference is slower than
content updates. It is not a guarantee of a frame deadline.

Size must remain stable for approximately 200 ms before a new worker is created.
Disable, failure and resize request worker shutdown and defer destruction until
it can complete without joining on the render thread. Explicit effect unloading
joins remaining workers once, so their code cannot outlive the plugin DSO.
SDK libraries use `RTLD_LOCAL` and remain pinned for the process lifetime because
CUDA/NGX may retain library references. A fresh login is required after binary
upgrades; unloading an effect does not ensure Qt has unmapped its library.

## Color and HDR

CAS precedes optional denoise/deblur, followed by TrueHDR, grain and destination
encoding. The SDR capture uses gamma 2.2. TrueHDR produces packed RGB10A2 in
BT.2020/PQ; the worker transfers it to RGBA16F without decoding PQ. KWin's color
shader then performs conversion and tone mapping. The original contribution
is transformed separately and blended in linear nits, with original alpha.

A geometry-less effect-owned scene item advertises generated HDR headroom to
KWin without relabeling application SDR pixels. Actual HDR output requires an
active HDR-enabled output. A separate SDR-look option permits KWin to tone map
the TrueHDR result to SDR. A window's main output chooses its pipeline; each
draw still receives destination color conversion. Grain can appear stronger
through the HDR-derived path and may need a lower strength.

## Limits and validation

The target is Linux x86-64 with compatible NVIDIA RTX hardware for SDK features;
Arch desktop integration is not a claim of official NVIDIA platform support.
The shader build works independently of the proprietary runtime. See the
[NVIDIA backend notes](../src/backends/nvidia/README.md) and
[packaging guide](packaging.md) for runtime setup and license handling.

Private-session tests cover selection, independent windows, resize/retirement,
shader output, worker transfer and tray controls. Sustained latency, effect
ordering and mixed-output visual quality need broader testing. An in-process
effect cannot recover the compositor from a fatal driver failure. Use the
[nested validation workflow](validation.md) before desktop deployment.
