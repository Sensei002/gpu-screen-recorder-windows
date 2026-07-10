# GPU Screen Recorder for Windows

A ShadowPlay-like GPU screen recorder for Windows with minimal performance impact. Records your screen using GPU hardware acceleration, similar to the Linux [gpu-screen-recorder](https://git.dec05eba.com/gpu-screen-recorder/about/) project.

## Features

- **🖥️ High-Performance Screen Capture** — Uses DXGI Desktop Duplication API for low-overhead screen capture
- **⚡ Hardware-Accelerated Encoding** — Supports NVIDIA NVENC, AMD AMF, and Intel QuickSync via FFmpeg
- **🔄 Instant Replay** — Ring buffer in RAM/disk, save the last N seconds with a hotkey
- **📡 Live Streaming** — Stream to Twitch, YouTube, or any RTMP/SRT server
- **🎮 ShadowPlay-Style Overlay UI** — DirectX overlay with ImGui, toggle with Alt+Z
- **🎵 WASAPI Audio Capture** — System audio loopback and microphone support
- **⌨️ Global Hotkeys** — Customizable keyboard shortcuts
- **📸 Screenshots** — Capture still images with PNG/JPEG output
- **🎨 HDR Support** — 10-bit HDR recording with HEVC/AV1

## Architecture

```
gpu-screen-recorder-windows/
├── src/
│   ├── capture/          # Screen (DXGI) + Audio (WASAPI) + Cursor capture
│   ├── encode/           # Video encoder, audio encoder, muxer (FFmpeg)
│   ├── replay/           # Ring buffer replay system
│   ├── recorder/         # Main orchestrator
│   ├── common/           # Types, logging, configuration
│   ├── cli/              # CLI tool (gpu-screen-recorder.exe)
│   └── ui/               # Overlay UI (gsr-ui.exe) with ImGui + DirectX 11
├── CMakeLists.txt        # CMake build system
└── README.md
```

## Building

### Prerequisites

1. **Visual Studio 2022** (or 2019) with C++ desktop development workload
2. **CMake** 3.20+
3. **FFmpeg** development libraries (x64)

### Option A: Build with vcpkg (Recommended)

```powershell
# Install vcpkg if not already installed
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat

# Install FFmpeg with hardware encoding support
vcpkg install ffmpeg[x264,x265,nvcodec,amf]:x64-windows

# Build the project
cd gpu-screen-recorder-windows
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Option B: Build with pre-installed FFmpeg

1. Download FFmpeg 7.0+ development binaries from [here](https://github.com/BtbN/FFmpeg-Builds/releases) (e.g., `ffmpeg-master-latest-win64-gpl-shared.zip`)
2. Extract to `C:\ffmpeg` and set environment variable: `FFMPEG_DIR=C:\ffmpeg`
3. Build:

```powershell
cmake -B build -DFFMPEG_DIR=C:/ffmpeg
cmake --build build --config Release
```

## Usage

### CLI Mode

```powershell
# Record your screen (with audio) to a file
gpu-screen-recorder -w screen -f 60 -a default_output -o video.mp4

# Instant replay mode (save last 30 seconds)
gpu-screen-recorder -w screen -f 60 -r 30 -o "C:\Videos\Replays"

# Use HEVC encoding with specific quality
gpu-screen-recorder -w screen -f 60 -k hevc -q 28 -o video.mp4

# Stream to Twitch
gpu-screen-recorder -w screen -f 60 -k h264 -b 4500 -a default_output -o "rtmp://live.twitch.tv/app/YOUR_STREAM_KEY"

# List available monitors
gpu-screen-recorder --list-capture-options

# List audio devices
gpu-screen-recorder --list-audio-devices
```

### UI Mode

```powershell
# Start the overlay UI
gsr-ui
```

The overlay appears as a floating transparent window. Control it with:
- **Alt+Z** — Toggle overlay visibility
- **F8** — Save replay
- **F9** — Start/Stop recording
- **Esc** — Hide overlay

### Remote Control (gsr-ui-cli)

The UI app can be controlled via named pipes (coming soon).

## Configuration

Config file location: `%APPDATA%\gsr\gsr.conf`

Key settings:
```
video_codec = hevc
video_fps = 60
video_bitrate = 20000
video_quality = 23
video_constant_quality = true
capture_cursor = true
capture_fps = 60
capture_audio = true
audio_codec = aac
audio_bitrate = 192
replay_duration = 30
replay_storage = ram
output_dir = C:\Users\<user>\Videos\GPU Screen Recorder
overlay_opacity = 90
theme = dark
```

## Comparison with Linux version

| Feature | Linux (gpu-screen-recorder) | Windows (this port) |
|---------|---------------------------|-------------------|
| Screen Capture | X11/Wayland + KMS/DRM | DXGI Desktop Duplication |
| Video Encoding | NVENC/VA-API | NVENC/AMF/QSV via FFmpeg |
| Audio Capture | PulseAudio/PipeWire | WASAPI loopback |
| Overlay UI | X11 overlay (ShadowPlay-style) | DirectX 11 + ImGui overlay |
| Instant Replay | Ring buffer (RAM/disk) | Ring buffer (RAM/disk) |
| Streaming | FFmpeg-based | FFmpeg-based |
| Global Hotkeys | evdev + virtual keyboard | Windows low-level hooks |
| IPC | Signals (SIGUSR1 etc.) | Named pipes (planned) |

## Technical Implementation

### Screen Capture
Uses `IDXGIOutputDuplication` (DXGI 1.2+) for high-performance screen capture. The Desktop Duplication API provides direct access to the GPU's framebuffer with minimal CPU overhead. Frames are captured in BGRA format and converted to NV12 for hardware encoding.

### Hardware Encoding
Auto-selects the best available hardware encoder:
1. **NVIDIA**: NVENC (h264_nvenc, hevc_nvenc, av1_nvenc)
2. **AMD**: AMF (h264_amf, hevc_amf)
3. **Intel**: QuickSync (h264_qsv, hevc_qsv)
4. Falls back to software encoding (libx264/libx265)

### Audio Capture
WASAPI loopback recording captures system audio. Supports low-latency mode via `IAudioClient3` on Windows 10+.

### Overlay UI
A transparent, click-through DirectX 11 window renders the ImGui-based interface. The overlay floats over all windows (topmost) and can be toggled with Alt+Z.

## License

GPL-3.0-only

This project is a from-scratch Windows implementation inspired by the Linux [gpu-screen-recorder](https://git.dec05eba.com/gpu-screen-recorder/) project by dec05eba.
