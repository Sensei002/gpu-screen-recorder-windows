# GPU Screen Recorder — Windows Port

A native Windows x64 port of the original **GPU Screen Recorder** project by
[dec05eba](https://git.dec05eba.com/gpu-screen-recorder/) — a screen recorder
with minimal system impact that records using the GPU only, similar to NVIDIA
ShadowPlay. This is **not** a new or "inspired-by" recorder: it is a genuine
platform port that preserves the upstream architecture, UI, UX, behavior, CLI
and project identity, and replaces only the Linux-specific platform layer
with native Windows implementations.

> **Status: Phases 1–14 complete (engine, UI, IPC, audio, replay, installer,
> CI/CD). Phases 15–17 — performance, parity testing, release packaging —
> are pending.** See [docs/implementation-roadmap.md](docs/implementation-roadmap.md)
> for the phase plan. Nothing is built locally — all compilation, testing,
> packaging and releasing happens in GitHub Actions (see
> [.github/workflows/windows-release.yml](.github/workflows/windows-release.yml)).

## What this is

* **Engine** (`gpu-screen-recorder.exe`) — the recording daemon: monitor /
  window capture, GPU encoding, instant replay, screenshots, streaming, CLI +
  IPC control. Port of upstream `gpu-screen-recorder` 6.0.0 (r1467/d31b698).
* **UI** (`gsr-ui.exe`, `gsr-ui-cli.exe`) — the ShadowPlay-style fullscreen
  overlay UI. Port of upstream `gpu-screen-recorder-ui` 1.13.5 (r720/edfc70d),
  including its custom OpenGL widget stack (mgl/mglpp).
* **Notification** (`gsr-notification.exe`) — **not implemented yet.** The
  upstream `gsr-notify` overlay has no Windows port in this tree: only
  `platform/include/notifications.h` exists (a declaration with no
  implementation), and the installer ships three executables
  (`gpu-screen-recorder`, `gsr-cli`, `gsr-ui`). The UI still spawns
  `gsr-notify`, so every toast currently fails silently. Engine output is
  logged to `%APPDATA%\gpu-screen-recorder\engine.log` as an interim
  diagnostic channel. See `docs/windows-port-parity.md`.
* The upstream GTK frontend (`gpu-screen-recorder-gtk`) is deprecated upstream
  and superseded by the UI; it is not ported (see
  [docs/windows-port-parity.md](docs/windows-port-parity.md)).

## Platform replacements at a glance

| Linux | Windows |
|---|---|
| X11 / Wayland / DRM-KMS capture | Windows Graphics Capture (primary) + DXGI Desktop Duplication (fallback) |
| VAAPI / Vulkan encoders | NVENC (NVIDIA) + software fallback — via FFmpeg; AMD/Intel encoders intentionally out of scope (NVIDIA-only port) |
| PulseAudio / PipeWire | WASAPI (loopback + capture) |
| POSIX signals / unix sockets | named events + named-pipe IPC (same JSON protocol) |
| systemd / XDG autostart | HKCU Run / Startup folder |
| mgl X11/Wayland windowing | mgl Win32 backend |
| evdev/uinput global hotkeys | RegisterHotKey / Raw Input |

See [docs/architecture.md](docs/architecture.md) for the full design and
[docs/windows-port-parity.md](docs/windows-port-parity.md) for the
feature-by-feature status matrix.

## Repository layout

```
platform/   Windows platform abstraction (capture, audio, ipc, display, ...)
src/        engine (upstream gpu-screen-recorder, modified minimally)
include/    engine headers
ui/         overlay UI + vendored mglpp/mgl
notify/     notification overlay
tools/      gsr-cli, gsr-ui-cli
installer/  setup EXE (built in CI)
tests/      automated tests (run in CI)
docs/       analysis, architecture, roadmap, licensing, parity, porting notes
.github/    the single CI/CD workflow
```

## Documentation

* [docs/upstream-analysis.md](docs/upstream-analysis.md) — full analysis of the upstream project.
* [docs/architecture.md](docs/architecture.md) — Windows port architecture.
* [docs/implementation-roadmap.md](docs/implementation-roadmap.md) — phase plan (Phases 2–20).
* [docs/windows-port-parity.md](docs/windows-port-parity.md) — functional parity matrix.
* [docs/licensing.md](docs/licensing.md) — license and attribution.
* [docs/upstream-porting-notes.md](docs/upstream-porting-notes.md) — upstream revision pins and sync strategy.
* [docs/build-windows.md](docs/build-windows.md) — build instructions (created in Phase 2).
* [docs/troubleshooting-windows.md](docs/troubleshooting-windows.md) — troubleshooting (created in later phases).

## License and attribution

This project is licensed under **GPL-3.0-only**, as required by the upstream
project. It is a modified version of GPU Screen Recorder by dec05eba and the
contributors; all upstream copyrights remain with their authors. See
[docs/licensing.md](docs/licensing.md) for the complete attribution and
third-party license list. Windows-specific modifications are tracked in
[docs/upstream-porting-notes.md](docs/upstream-porting-notes.md).

**Upstream:** <https://git.dec05eba.com/gpu-screen-recorder/> ·
<https://git.dec05eba.com/gpu-screen-recorder-ui/> ·
<https://git.dec05eba.com/gpu-screen-recorder-gtk/> ·
<https://git.dec05eba.com/gpu-screen-recorder-notification/>
