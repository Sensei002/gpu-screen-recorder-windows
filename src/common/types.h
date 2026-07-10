#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

// ─── Video ───────────────────────────────────────────────────────────────────

enum class VideoCodec {
    H264,
    HEVC,
    AV1,
    VP8,
    VP9,
};

enum class PixelFormat {
    NV12,
    I420,
    BGRA,
    RGBA,
    P010, // 10-bit HDR
};

enum class ColorRange {
    Limited, // 16-235
    Full,    // 0-255
};

enum class ColorSpace {
    BT709,
    BT2020,
};

enum class CaptureMethod {
    DXGI,
    GraphicsCapture, // Windows.Graphics.Capture
};

enum class FrameCaptureMode {
    FixedFPS,  // Capture at fixed interval
    Content,   // Sync to content updates (like -fm content on Linux)
    Variable,  // Variable framerate
};

struct VideoFrame {
    uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    PixelFormat format = PixelFormat::BGRA;
    int64_t timestamp_us = 0; // Microsecond timestamp
    bool is_cursor_overlay = false;
};

struct VideoEncoderConfig {
    VideoCodec codec = VideoCodec::H264;
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrate_kbps = 20000;
    bool constant_quality = true;
    int quality = 23; // CRF value
    bool enable_hdr = false;
    ColorSpace color_space = ColorSpace::BT709;
    bool enable_10bit = false;
    int keyint_seconds = 2; // Keyframe interval in seconds
    std::string encoder_name; // Empty = auto-select best available
};

// ─── Audio ───────────────────────────────────────────────────────────────────

enum class AudioCodec {
    AAC,
    Opus,
    MP3,
    PCM,
};

enum class AudioSourceType {
    DefaultOutput, // System audio (what you hear)
    Microphone,
    Application,   // Specific application audio
    Both,          // Mix of system + mic
};

struct AudioFrame {
    uint8_t* data = nullptr;
    int samples = 0;
    int channels = 0;
    int sample_rate = 0;
    int64_t timestamp_us = 0;
};

struct AudioEncoderConfig {
    AudioCodec codec = AudioCodec::AAC;
    int sample_rate = 48000;
    int channels = 2;
    int bitrate_kbps = 192;
    AudioSourceType source_type = AudioSourceType::DefaultOutput;
    std::string application_name;
};

// ─── Capture ─────────────────────────────────────────────────────────────────

struct MonitorInfo {
    std::string name;      // e.g., "DISPLAY1", "DP-1"
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    int refresh_rate = 60;
    bool is_primary = false;
    uint32_t adapter_index = 0;
    uint32_t output_index = 0;
};

struct AudioDeviceInfo {
    std::string name;
    std::string id;
    int default_channels = 2;
    int default_sample_rate = 48000;
    bool is_default_output = false;
    bool is_default_input = false;
};

struct CaptureConfig {
    std::string monitor_name; // Empty = primary monitor
    int width = 0;           // 0 = monitor's native width
    int height = 0;          // 0 = monitor's native height
    int fps = 60;
    FrameCaptureMode capture_mode = FrameCaptureMode::FixedFPS;
    bool capture_cursor = true;
    bool capture_audio = true;
    AudioEncoderConfig audio_config;
};

// ─── Output ──────────────────────────────────────────────────────────────────

enum class OutputMode {
    File,     // Record to file
    Replay,   // Instant replay (ring buffer)
    Stream,   // Live streaming
    ReplayAndRecord, // Both replay and recording simultaneously
};

enum class RecordingState {
    Idle,
    Recording,
    ReplayActive,
    Streaming,
    Paused,
    Error,
};

// ─── Replay Buffer ───────────────────────────────────────────────────────────

enum class ReplayStorage {
    RAM,   // Store in memory
    Disk,  // Store on disk
};

// ─── Results / Callbacks ─────────────────────────────────────────────────────

struct RecordingResult {
    bool success = false;
    std::string file_path;
    std::string error_message;
    int64_t duration_ms = 0;
    int64_t file_size_bytes = 0;
    VideoCodec video_codec = VideoCodec::H264;
    AudioCodec audio_codec = AudioCodec::AAC;
    int width = 0;
    int height = 0;
    int fps = 0;
};

// Callback types
using FrameCallback = std::function<void(const VideoFrame&)>;
using AudioFrameCallback = std::function<void(const AudioFrame&)>;
using RecordingCompleteCallback = std::function<void(const RecordingResult&)>;
using ErrorCallback = std::function<void(const std::string& error)>;
using LogCallback = std::function<void(const std::string& message, int level)>;

// ─── Config ──────────────────────────────────────────────────────────────────

struct RecorderConfig {
    // Output
    OutputMode output_mode = OutputMode::File;
    std::string output_path;        // File path or directory (for replay)
    std::string output_directory;   // For replay mode
    std::string stream_url;         // For streaming

    // Capture
    CaptureConfig capture;

    // Video Encoding
    VideoEncoderConfig video;

    // Replay
    int replay_duration_seconds = 30;
    ReplayStorage replay_storage = ReplayStorage::RAM;

    // Recording while replaying
    bool record_while_replay = false;
    std::string replay_output_directory;

    // Scripts
    std::string save_script;        // Script to run on save (first arg = file path, second = type)

    // Performance
    bool enable_performance_monitoring = true;
    int log_level = 1; // 0=error, 1=warn, 2=info, 3=debug
};

// ─── Controller API (for UI/remote control) ──────────────────────────────────

enum class ControllerCommand {
    StartRecording,
    StopRecording,
    SaveReplay,
    CancelReplay,
    PauseRecording,
    ResumeRecording,
    ShowUI,
    HideUI,
    ToggleUI,
    StartStream,
    StopStream,
    TakeScreenshot,
    SetConfig,
    QueryStatus,
    Quit,
};

struct ControllerMessage {
    ControllerCommand command;
    std::string data; // JSON payload
    uint32_t message_id = 0;
};

struct ControllerResponse {
    bool success = false;
    uint32_t message_id = 0;
    std::string data; // JSON payload
    std::string error;
};

// ─── Screenshot ──────────────────────────────────────────────────────────────

enum class ImageFormat {
    JPEG,
    PNG,
};

struct ScreenshotConfig {
    ImageFormat format = ImageFormat::PNG;
    int quality = 95; // JPEG quality
    std::string output_path;
};

// ─── Plugin API ──────────────────────────────────────────────────────────────

struct PluginInfo {
    std::string name;
    std::string version;
    std::string description;
};

// ─── Utility ─────────────────────────────────────────────────────────────────

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

inline std::string videoCodecToString(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::H264: return "h264";
        case VideoCodec::HEVC: return "hevc";
        case VideoCodec::AV1:  return "av1";
        case VideoCodec::VP8:  return "vp8";
        case VideoCodec::VP9:  return "vp9";
        default:               return "unknown";
    }
}

inline std::string audioCodecToString(AudioCodec codec) {
    switch (codec) {
        case AudioCodec::AAC:  return "aac";
        case AudioCodec::Opus: return "opus";
        case AudioCodec::MP3:  return "mp3";
        case AudioCodec::PCM:  return "pcm";
        default:               return "unknown";
    }
}
