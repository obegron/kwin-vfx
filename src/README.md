# Source layout

- `effect/`: KWin lifecycle, per-window capture and selection, shortcuts,
  D-Bus controls, color-aware drawing, fallback and worker retirement.
- `backends/shader/`: spatial denoise/sharpen and AMD CAS GLSL, shared with the
  standalone shader correctness and benchmark harness.
- `backends/nvidia/`: optional native CUDA/EGL worker, runtime bindings and
  standalone interop/inference probes. No proprietary libraries are vendored.
- `preview/`: Qt system tray controls for the desktop or a private test session.

The effect owns its captured textures. Shader filtering runs during the final
window draw; NVIDIA inference runs on a separate worker with explicit GL/CUDA
ownership transfers. See [architecture](../docs/architecture.md) and
[validation](../docs/validation.md).
