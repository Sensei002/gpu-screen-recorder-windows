# Functional Parity Matrix — Linux Upstream vs Windows Port

Status legend:

* **FULL** — implemented with equivalent behavior.
* **PARTIAL** — implemented with a documented difference.
* **WINDOWS-SPECIFIC** — Windows-only behavior added where the platform
  requires it (no upstream equivalent).
* **NOT POSSIBLE** — no faithful Windows equivalent; feature is hidden with a
  clear error (never faked).
* **TODO** — planned, not yet implemented.

This matrix is a living document; Phase 16 (parity testing) drives every row
to a final status. `PLANNED` rows below reflect the architecture in
`docs/architecture.md`.

## Engine — CLI and control

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| CLI `-w` capture source syntax (`screen`, monitor name, `window`, `region`, `portal`, `v4l2`, `|` composition, `;x=...;y=...` options) | FULL | Monitor/`screen`/`region`/window via WGC; no `portal`; no v4l2 yet | PARTIAL → TODO | Webcam scope decided in Phase 5/13 |
| `-c` container (mp4/mkv/flv/webm + FFmpeg formats) | FULL | FULL | PLANNED | FFmpeg muxing |
| `-s` resolution limit | FULL | FULL | PLANNED | GPU scaling |
| `-f` fps + `-fm cfr/vfr` | FULL | FULL | FULL | `auto` maps to **cfr** on Windows (VFR on a polled capture drops ~9% of frames and jitters: 54.5 fps with 300+ distinct intervals vs 60.00 fps at a fixed 16.67 ms). Explicit `-fm vfr` is still honoured |
| `-fm content` (damage sync) | X11/portal only | WGC-native equivalent (capture-on-change) | PARTIAL | WGC delivers frames on desktop update; map to damage model |
| `-cursor yes/no` | FULL | FULL | PLANNED | WGC cursor API |
| `-a` audio sources (`default_output`, `default_input`, `device:`, `app:`, `app-inverse:`, `\|`) | FULL | device/default FULL; `app:` PARTIAL | PLANNED | WASAPI sessions; feasibility Phase 8 |
| `-ac`/`-ab` (opus/aac; flac disabled upstream) | FULL | FULL | PLANNED | keep flac disabled for parity |
| `-k` codecs (h264/hevc/av1/vp8/vp9 + `_hdr`/`_10bit` + `_vulkan`) | FULL | h264/hevc/av1 via nvenc/software; vp8/vp9 via software | PARTIAL | NVIDIA-only port; `_vulkan` variants NOT POSSIBLE unless Vulkan encode kept; hidden |
| `-q`/`-bm` (qp/vbr/cbr/auto) | FULL | FULL | PLANNED | FFmpeg encoders support these |
| `-cr` color range | FULL | FULL | PLANNED | |
| `-tune` performance/quality | NVIDIA only | NVIDIA (nvenc) | PARTIAL | same scope |
| `-keyint` | FULL | FULL | PLANNED | |
| `-encoder gpu/cpu` + `-fallback-cpu-encoding` | FULL | FULL | PLANNED | cpu = libx264 only, as upstream |
| `-r` replay + `-replay-storage ram/disk` + `-restart-replay-on-save` + `-df` | FULL | FULL | PLANNED | portable replay code |
| `-p` plugins | FULL (.so, GL) | `.dll`; render backend per architecture §3.4 | PARTIAL | plugin.h ABI kept |
| `-sc` script on save | FULL (shell) | `cmd /c` / direct exe / ps1 | PARTIAL | documented |
| `-ffmpeg-opts` / `-ffmpeg-video-opts` / `-ffmpeg-audio-opts` | FULL | FULL | PLANNED | |
| `-gl-debug` | FULL | n/a or WGL debug | WINDOWS-SPECIFIC | debug output for Windows GL/D3D |
| `-v` (fps/damage info) | FULL | FULL | PLANNED | |
| `-low-power` | AMD (VAAPI) | NOT PLANNED (AMD dropped) | NOT POSSIBLE | NVIDIA-only port; error if requested |
| `-exclude-metadata` | FULL | FULL | PLANNED | |
| `-write-first-frame-ts` | FULL | FULL | PLANNED | QPC-based |
| `-ipc` unix socket | FULL | named pipe | WINDOWS-SPECIFIC | same JSON protocol |
| Signals (SIGUSR1/2, SIGRTMIN+n) | FULL | named events + `gsr-cli` | WINDOWS-SPECIFIC | semantics preserved; UI sends control via gsr-cli (`save-replay [n]`, `toggle-pause`, `toggle-replay-recording`, `stop`) instead of kill(). Engine shutdown uses a bounded wait (15 s) with a TerminateProcess fallback so a stuck engine can't hang the UI |
| `gsr-cli` | FULL | `gsr-cli.exe` | PLANNED | same CLI/exit codes |
| `--help`/`--version`/`--info`/`--list-*` | FULL | FULL (same `key|value` format) | PLANNED | `--info` gains Windows fields (backend, DXGI info) |
| Exit codes per error class | PARTIAL upstream | PARTIAL | PLANNED | keep upstream's mapping |
| stdout=paths / stderr=logs contract | FULL | FULL | DONE | UI reads engine stdout via the Windows pipe (`PeekNamedPipe`/`ReadFile`); record/replay stop + save-replay filepaths surface like Linux. UI config parser strips a trailing `\r` from CRLF `config_ui` values so save directories/booleans reach the engine clean |

## Engine — capture

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| Monitor capture | KMS (root), NvFBC, portal | WGC monitor item (no elevation) | PLANNED | primary backend |
| Fallback capture | — | DXGI Desktop Duplication | WINDOWS-SPECIFIC | |
| Window capture | XComposite / portal | WGC window item | PLANNED | |
| `-w screen-direct` (NVIDIA X11 VRR) | NVIDIA X11 | NOT POSSIBLE (WGC handles VRR) | NOT POSSIBLE | clear error |
| `-w focused` | X11 | `GetForegroundWindow` + WGC window item | PARTIAL | |
| `-w portal` | Wayland | WGC (equivalent capability) | WINDOWS-SPECIFIC | no portal protocol on Windows |
| `-w region` | X11/Wayland + slop/slurp | Win32 region selector overlay | PLANNED | |
| Webcam (`-w /dev/video0`) | V4L2 | Media Foundation (scope decision) | TODO | |
| Multi-monitor / mixed DPI / rotation | FULL | FULL | PLANNED | DXGI enumeration |
| High refresh / VRR | FULL (with caveats) | FULL (WGC) | PLANNED | |
| HDR capture + HEVC/AV1 HDR metadata | FULL (Wayland portal limitations) | PARTIAL | PLANNED | DXGI color space; document tonemapping |
| Cursor (shape/pos/high-DPI) | FULL | FULL | PLANNED | WGC cursor |
| Damage tracking (content mode) | X11 XDamage | WGC frame cadence | PARTIAL | |
| Device loss / mode change recovery | TODO upstream | recreate capture session | PLANNED | |

## Engine — encoding

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| NVENC h264/hevc/av1 | FULL | FULL via FFmpeg d3d11va + nvenc | PLANNED | capability-gated |
| AMF (AMD) | n/a (VAAPI) | NOT PLANNED | NOT POSSIBLE | dropped — NVIDIA-only scope decision (2026-08-15) |
| QSV (Intel) | n/a (VAAPI) | NOT PLANNED | NOT POSSIBLE | dropped — NVIDIA-only scope decision (2026-08-15) |
| VAAPI | FULL | NOT POSSIBLE | NOT POSSIBLE | replaced by NVENC (NVIDIA-only port) |
| Vulkan video encode | experimental | TODO decision | TODO | keep if feasible, else hidden |
| Software encode (libx264) | FULL | FULL | PLANNED | h264 only, as upstream |
| Old NVIDIA GPUs (GTX 9xx/10xx etc.) | FULL (flatpak ffmpeg patch) | PARTIAL | PLANNED | capability detection + documented limits; AV1 hidden pre-RTX |
| HDR/10-bit encode | FULL | PARTIAL | PLANNED | only when pipeline delivers 10-bit |
| Presets/tuning/bitrate modes | FULL | FULL | PLANNED | |

## Engine — audio

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| Desktop audio | PulseAudio/PipeWire | WASAPI loopback | PLANNED | |
| Microphone | PulseAudio | WASAPI capture | PLANNED | |
| Device by name + follow default | FULL | FULL | PLANNED | MMNotificationClient |
| App audio (`-a app:`) | PipeWire | WASAPI session capture (feasibility) | TODO | documented fallback if not feasible |
| Multiple sources / mixing (amix) | FULL | FULL | PLANNED | libswresample + amix, portable |
| Volume/mute control | via pavucontrol (external) | n/a — same (external) | PARTIAL | upstream has no built-in volume |
| Audio device change handling | TODO upstream | error/recovery path | PLANNED | |
| A/V sync (no frame-count assumptions) | FULL | FULL | PLANNED | QPC clock + codec delay offsets |

## Engine — replay / screenshot / streaming

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| Instant replay (continuous encoded buffer) | FULL | FULL | PLANNED | portable |
| Replay RAM/disk storage | FULL | FULL | PLANNED | |
| Save N seconds / full / restart-on-save | FULL | FULL | PLANNED | |
| Crash-safe disk buffer | PARTIAL upstream | FULL (cleanup on start) | PLANNED | upstream TODO implemented |
| Screenshot (jpg/png) | FULL | FULL | PLANNED | WGC capture + image_writer |
| Screenshot to clipboard | UI-side (X11) | Win32 clipboard | PLANNED | |
| Streaming (RTMP/SRT/WHIP/HLS/etc.) | FULL | FULL | PLANNED | FFmpeg protocols |
| Record while replay/streaming (`-ro`) | FULL | FULL | PLANNED | multi-destination encoder |

## UI (gsr-ui)

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| Overlay UI (ShadowPlay style) | FULL | FULL | PLANNED | mgl Win32 backend |
| Alt+Z show/hide + configurable hotkeys | FULL | FULL | PLANNED | RegisterHotKey |
| Front page (record/replay/stream/screenshot/region/window) | FULL | FULL | PLANNED | |
| Replay save buttons (full/1min/10min) | FULL | FULL | PLANNED | |
| Settings pages (codec/quality/fps/audio/dirs/hotkeys/...) | FULL | FULL | PLANNED | config_ui schema kept |
| Profiles | FULL | FULL | PLANNED | |
| Webcam overlay | FULL | TODO | TODO | Media Foundation scope |
| Game-name folders (focused window title) | X11 + DE extensions | foreground window/process | PARTIAL | no shell extensions |
| Region/window selection UI | X11/Wayland | Win32 overlay | PLANNED | |
| Overlay background = captured window | X11 XComposite | WGC window preview | PARTIAL | |
| Controller (joystick) hotkeys | evdev/jsN | XInput | PARTIAL | |
| System tray | TODO upstream (in gtk frontend) | minimal tray icon (decision Phase 10) | WINDOWS-SPECIFIC | |
| Start on system startup | XDG autostart | HKCU Run / Startup folder | WINDOWS-SPECIFIC | |
| Notifications | gsr-notify process | gsr-notification.exe | PLANNED | same CLI |
| Translations | FULL | FULL | PLANNED | assets carried over |
| Clipboard (screenshot copy) | X11/Wayland | Win32 clipboard | PLANNED | works while unfocused on Windows |
| RPC (gsr-ui socket) | abstract unix socket | named pipe | WINDOWS-SPECIFIC | same commands |
| gsr-ui-cli | FULL | `gsr-ui-cli.exe` | PLANNED | |

## GTK frontend / notification / helpers

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| gpu-screen-recorder-gtk (tray app) | deprecated upstream | NOT ported (superseded by UI) | NOT POSSIBLE | documented; tray scope lives in UI |
| gsr-notify → `gsr-notification.exe` | FULL | **missing** — no source in tree; UI still spawns `gsr-notify` | TODO | Every toast fails at `CreateProcess`; engine output goes to `%APPDATA%\gpu-screen-recorder\engine.log` instead. Needs either a port of `gsr-notify` or in-overlay toasts |
| gsr-global-hotkeys (setuid helper) | FULL | in-process RegisterHotKey | WINDOWS-SPECIFIC | no helper binary needed |
| gsr-wayland-bridge / kwin / gnome helpers | Wayland-only | NOT POSSIBLE | NOT POSSIBLE | replaced by Win32 equivalents |
| gsr-game-tracker | FULL | foreground-window based | PARTIAL | |

## Platform / system integration

| Feature | Linux Upstream | Windows | Status | Notes |
|---|---|---|---|---|
| No admin needed | PARTIAL (KMS root helper) | FULL (WGC user-mode) | WINDOWS-SPECIFIC | improvement over upstream |
| Startup mechanism | systemd user service / XDG autostart | HKCU Run / Startup folder | WINDOWS-SPECIFIC | |
| Config paths | `~/.config/gpu-screen-recorder` | `%APPDATA%\gpu-screen-recorder` | WINDOWS-SPECIFIC | schema preserved |
| Save dir default | `~/Videos` | `%USERPROFILE%\Videos` | WINDOWS-SPECIFIC | |
| Unicode paths | FULL | FULL (wide chars) | PLANNED | |
| Installer | distro packages | Setup.exe (NSIS/Inno/WiX) | WINDOWS-SPECIFIC | |
| Portable zip | n/a | FULL | WINDOWS-SPECIFIC | |
| GitHub Actions CI/CD | n/a | FULL (single workflow) | WINDOWS-SPECIFIC | |

## Explicitly out of scope (never implemented)

* Telemetry, analytics, ads, accounts, cloud dependency.
* Electron/WebView/React/Next.js UI.
* OBS dependency.
* Kernel drivers, DLL injection, anti-cheat bypass.
* Any "inspired-by" rebranding — the project keeps its upstream identity.
