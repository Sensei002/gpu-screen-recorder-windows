// GPU Screen Recorder - Windows CLI
// A ShadowPlay-like screen recorder for Windows with minimal performance impact.
// Usage: gpu-screen-recorder [options]

#include "common/log.h"
#include "common/config.h"
#include "common/types.h"
#include "recorder/recorder.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>
#include <iostream>

// ─── Global State ───────────────────────────────────────────────────────────

static std::unique_ptr<Recorder> g_recorder;
static std::atomic<bool> g_should_exit{false};
static std::string g_output_path;

// ─── Signal Handling ────────────────────────────────────────────────────────

void signal_handler(int signal) {
    switch (signal) {
        case SIGINT:  // Ctrl+C - Stop and save
            LOG_INFO("SIGINT received - stopping recording and saving");
            if (g_recorder) {
                g_recorder->stop();
            }
            g_should_exit = true;
            break;

        case SIGUSR1:
#ifdef _WIN32
            // On Windows, SIGUSR1 is not available. Use a console event handler instead.
            // We'll handle this via named pipe in the full implementation.
#else
            LOG_INFO("SIGUSR1 received - saving replay");
            if (g_recorder) {
                g_recorder->save_replay();
            }
#endif
            break;

        default:
            break;
    }
}

#ifdef _WIN32
BOOL WINAPI console_event_handler(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            LOG_INFO("Console event received - stopping");
            if (g_recorder) {
                g_recorder->stop();
            }
            g_should_exit = true;
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

// ─── Argument Parsing ───────────────────────────────────────────────────────

struct CLIArgs {
    // General
    bool show_help = false;
    bool show_version = false;

    // Capture options
    std::string monitor_name;       // -w monitor:<name> or screen, window, portal
    int fps = 60;                   // -f
    FrameCaptureMode capture_mode = FrameCaptureMode::FixedFPS; // -fm

    // Video options
    VideoCodec video_codec = VideoCodec::H264; // -k
    std::string encoder_name;       // -enc
    int video_quality = 23;         // -q
    bool constant_quality = true;
    int bitrate_kbps = 20000;       // -bm ... bitrate
    std::string bitrate_mode_str;   // -bm [cbr, vbr, cq]
    bool hdr_enabled = false;
    int keyint = 2;                 // -keyint

    // Audio options
    std::string audio_device;       // -a default_output, default_input, or device name
    AudioCodec audio_codec = AudioCodec::AAC; // -ac
    int audio_bitrate = 192;        // -ab

    // Output options
    std::string output;             // -o (file path, directory for replay, or stream URL)
    bool replay_mode = false;       // -r (replay duration in seconds)
    int replay_duration = 30;
    std::string replay_storage_str; // -replay-storage [ram, disk]
    bool record_while_replay = false;

    // Other
    bool list_capture_options = false;
    bool list_audio_devices = false;
    bool print_info = false;
    int log_level = 1;

    // Config
    std::string config_file;
    std::string save_script;        // -sc
};

void print_usage() {
    printf("GPU Screen Recorder v1.0.0 - Windows Edition\n");
    printf("A ShadowPlay-like GPU screen recorder for Windows.\n");
    printf("\n");
    printf("Usage: gpu-screen-recorder [options]\n");
    printf("\n");
    printf("Capture options:\n");
    printf("  -w <window|screen|monitor:name>   What to capture (default: screen)\n");
    printf("  -f <fps>                          Framerate (default: 60)\n");
    printf("  -fm <fixed|content|vfr>           Frame capture mode (default: fixed)\n");
    printf("\n");
    printf("Video options:\n");
    printf("  -k <h264|hevc|av1|vp8|vp9>        Video codec (default: h264)\n");
    printf("  -q <quality>                      Video quality 0-51 (default: 23, lower=better)\n");
    printf("  -bm <cq|cbr|vbr>                  Bitrate mode (default: cq)\n");
    printf("  -b <bitrate>                      Bitrate in kbps (default: 20000)\n");
    printf("  -enc <encoder>                    Encoder name (default: auto-select)\n");
    printf("  -keyint <seconds>                 Keyframe interval (default: 2)\n");
    printf("  -hdr                              Enable HDR recording\n");
    printf("\n");
    printf("Audio options:\n");
    printf("  -a <device>                       Audio source (default_output|default_input|name)\n");
    printf("  -ac <aac|opus>                    Audio codec (default: aac)\n");
    printf("  -ab <bitrate>                     Audio bitrate in kbps (default: 192)\n");
    printf("\n");
    printf("Output options:\n");
    printf("  -o <path>                         Output file or directory (for replay)\n");
    printf("  -r <seconds>                      Replay mode with buffer duration\n");
    printf("  -replay-storage <ram|disk>        Replay storage type (default: ram)\n");
    printf("  -ro <directory>                   Recording directory (record while replaying)\n");
    printf("  -sc <script>                      Script to run on video save\n");
    printf("\n");
    printf("Other:\n");
    printf("  -h, --help                        Show this help\n");
    printf("  -v, --version                     Show version\n");
    printf("  --list-capture-options            List available capture options\n");
    printf("  --list-audio-devices              List available audio devices\n");
    printf("  --info                            Show system info\n");
    printf("  -l <level>                        Log level 0-3 (default: 1)\n");
    printf("  -c <format>                       Output container format (implied by extension)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  gpu-screen-recorder -w screen -f 60 -a default_output -o video.mp4\n");
    printf("  gpu-screen-recorder -w screen -f 144 -r 60 -c mp4 -o \"C:\\Videos\\Replays\"\n");
    printf("  gpu-screen-recorder -w screen -f 60 -k hevc -q 28 -o video.mp4\n");
    printf("  gpu-screen-recorder -w screen -k h264 -b 20000 -bm cbr -o video.mp4\n");
    printf("\n");
    printf("Remote control:\n");
    printf("  Send SIGINT (Ctrl+C) to stop recording and save\n");
    printf("  Signal SIGUSR1 to save replay (via gsr-ui-cli)\n");
}

void print_version() {
    printf("GPU Screen Recorder v1.0.0\n");
    printf("Windows Edition\n");
    printf("Copyright (C) 2024-2026\n");
    printf("This is free software; see the source for copying conditions.\n");
}

bool parse_args(int argc, char** argv, CLIArgs& args) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            args.show_help = true;
            return true;
        }

        if (arg == "-v" || arg == "--version") {
            args.show_version = true;
            return true;
        }

        if (arg == "--list-capture-options") {
            args.list_capture_options = true;
            return true;
        }

        if (arg == "--list-audio-devices") {
            args.list_audio_devices = true;
            return true;
        }

        if (arg == "--info") {
            args.print_info = true;
            return true;
        }

        if (arg == "-w" && i + 1 < argc) {
            args.monitor_name = argv[++i];
            continue;
        }

        if (arg == "-f" && i + 1 < argc) {
            args.fps = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-fm" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "content") args.capture_mode = FrameCaptureMode::Content;
            else if (mode == "vfr") args.capture_mode = FrameCaptureMode::Variable;
            else args.capture_mode = FrameCaptureMode::FixedFPS;
            continue;
        }

        if (arg == "-k" && i + 1 < argc) {
            std::string codec = argv[++i];
            if (codec == "h264") args.video_codec = VideoCodec::H264;
            else if (codec == "hevc") args.video_codec = VideoCodec::HEVC;
            else if (codec == "av1") args.video_codec = VideoCodec::AV1;
            else if (codec == "vp8") args.video_codec = VideoCodec::VP8;
            else if (codec == "vp9") args.video_codec = VideoCodec::VP9;
            else {
                fprintf(stderr, "Unknown video codec: %s\n", codec.c_str());
                return false;
            }
            continue;
        }

        if (arg == "-q" && i + 1 < argc) {
            args.video_quality = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-bm" && i + 1 < argc) {
            args.bitrate_mode_str = argv[++i];
            if (args.bitrate_mode_str == "cbr") {
                args.constant_quality = false;
            } else if (args.bitrate_mode_str == "vbr") {
                args.constant_quality = false;
            } else {
                args.constant_quality = true;
            }
            continue;
        }

        if (arg == "-b" && i + 1 < argc) {
            args.bitrate_kbps = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-enc" && i + 1 < argc) {
            args.encoder_name = argv[++i];
            continue;
        }

        if (arg == "-keyint" && i + 1 < argc) {
            args.keyint = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-hdr") {
            args.hdr_enabled = true;
            continue;
        }

        if (arg == "-a" && i + 1 < argc) {
            args.audio_device = argv[++i];
            continue;
        }

        if (arg == "-ac" && i + 1 < argc) {
            std::string codec = argv[++i];
            if (codec == "aac") args.audio_codec = AudioCodec::AAC;
            else if (codec == "opus") args.audio_codec = AudioCodec::Opus;
            else {
                fprintf(stderr, "Unknown audio codec: %s\n", codec.c_str());
                return false;
            }
            continue;
        }

        if (arg == "-ab" && i + 1 < argc) {
            args.audio_bitrate = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-o" && i + 1 < argc) {
            args.output = argv[++i];
            continue;
        }

        if (arg == "-r" && i + 1 < argc) {
            args.replay_mode = true;
            args.replay_duration = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-replay-storage" && i + 1 < argc) {
            args.replay_storage_str = argv[++i];
            continue;
        }

        if (arg == "-ro" && i + 1 < argc) {
            args.record_while_replay = true;
            args.output = argv[++i];
            continue;
        }

        if (arg == "-sc" && i + 1 < argc) {
            args.save_script = argv[++i];
            continue;
        }

        if (arg == "-l" && i + 1 < argc) {
            args.log_level = std::stoi(argv[++i]);
            continue;
        }

        if (arg == "-c" && i + 1 < argc) {
            // Container format - skip, inferred from extension
            ++i;
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
        fprintf(stderr, "Use -h for help\n");
        return false;
    }

    return true;
}

// ─── List Capture Options ───────────────────────────────────────────────────

void list_capture_options() {
    auto monitors = Recorder::list_monitors();
    printf("Capture options:\n");
    printf("  screen                    Record the entire desktop\n");
    printf("  window                    Record a selected window\n");
    for (const auto& monitor : monitors) {
        printf("  %-24s %dx%d @ %d Hz%s\n",
               monitor.name.c_str(), monitor.width, monitor.height,
               monitor.refresh_rate,
               monitor.is_primary ? " (primary)" : "");
    }
}

void list_audio_devices() {
    auto devices = Recorder::list_audio_devices();
    printf("Audio devices:\n");
    for (const auto& device : devices) {
        printf("  %-32s %s\n",
               device.name.c_str(),
               device.is_default_output ? "(default output)" : "");
    }
    printf("\n");
    printf("Special devices:\n");
    printf("  default_output            Default system audio output\n");
    printf("  default_input             Default microphone\n");
}

void print_system_info() {
    printf("GPU Screen Recorder v1.0.0 - Windows Edition\n");
    printf("\n");
    printf("System Information:\n");
    printf("  OS: Windows\n");
    printf("  Architecture: x64\n");
    printf("\n");

    auto monitors = Recorder::list_monitors();
    printf("Monitors:\n");
    for (const auto& m : monitors) {
        printf("  %s: %dx%d @ %d Hz%s\n",
               m.name.c_str(), m.width, m.height, m.refresh_rate,
               m.is_primary ? " (primary)" : "");
    }
    printf("\n");

    printf("Available video encoders:\n");
    auto encoders = VideoEncoder::list_available_encoders();
    for (const auto& enc : encoders) {
        printf("  %s\n", enc.c_str());
    }
    printf("\n");

    printf("Hardware encoder availability:\n");
    printf("  NVENC (NVIDIA):   %s\n",
           VideoEncoder::is_hardware_encoder_available("h264_nvenc") ? "Available" : "Not found");
    printf("  AMF (AMD):       %s\n",
           VideoEncoder::is_hardware_encoder_available("h264_amf") ? "Available" : "Not found");
    printf("  QSV (Intel):     %s\n",
           VideoEncoder::is_hardware_encoder_available("h264_qsv") ? "Available" : "Not found");
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Set up console event handler for Windows
#ifdef _WIN32
    SetConsoleCtrlHandler(console_event_handler, TRUE);
    // Enable ANSI escape codes
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
    }
#endif

    // Parse arguments
    CLIArgs args;
    if (!parse_args(argc, argv, args)) {
        return 1;
    }

    if (args.show_help) {
        print_usage();
        return 0;
    }

    if (args.show_version) {
        print_version();
        return 0;
    }

    if (args.list_capture_options) {
        list_capture_options();
        return 0;
    }

    if (args.list_audio_devices) {
        list_audio_devices();
        return 0;
    }

    if (args.print_info) {
        print_system_info();
        return 0;
    }

    // Initialize logger
    Logger::instance().set_level(static_cast<LogLevel>(std::min(args.log_level, 4)));

    // Load config
    auto& config = Config::instance();
    config.load(args.config_file);
    auto& recorder_config = config.recorder();

    // Apply CLI args to config
    recorder_config.video.codec = args.video_codec;
    recorder_config.video.fps = args.fps;
    recorder_config.video.quality = args.video_quality;
    recorder_config.video.constant_quality = args.constant_quality;
    recorder_config.video.bitrate_kbps = args.bitrate_kbps;
    recorder_config.video.enable_hdr = args.hdr_enabled;
    recorder_config.video.keyint_seconds = args.keyint;

    if (!args.encoder_name.empty()) {
        recorder_config.video.encoder_name = args.encoder_name;
    }

    recorder_config.capture.fps = args.fps;
    recorder_config.capture.capture_mode = args.capture_mode;
    recorder_config.capture.monitor_name = args.monitor_name;

    recorder_config.capture.audio_config.codec = args.audio_codec;
    recorder_config.capture.audio_config.bitrate_kbps = args.audio_bitrate;
    if (!args.audio_device.empty()) {
        if (args.audio_device == "default_input") {
            recorder_config.capture.audio_config.source_type = AudioSourceType::Microphone;
        } else {
            recorder_config.capture.audio_config.source_type = AudioSourceType::DefaultOutput;
        }
    }

    recorder_config.output_path = args.output;
    recorder_config.replay_duration_seconds = args.replay_duration;
    if (args.replay_storage_str == "disk") {
        recorder_config.replay_storage = ReplayStorage::Disk;
    } else {
        recorder_config.replay_storage = ReplayStorage::RAM;
    }

    if (!args.save_script.empty()) {
        recorder_config.save_script = args.save_script;
    }

    // Create recorder
    g_recorder = std::make_unique<Recorder>();

    // Set up callbacks
    g_recorder->set_status_callback([](const std::string& status, int progress) {
        LOG_INFO("Status: %s", status.c_str());
    });

    g_recorder->set_error_callback([](const std::string& error) {
        LOG_ERROR("Error: %s", error.c_str());
        fprintf(stderr, "Error: %s\n", error.c_str());
    });

    g_recorder->set_complete_callback([](const RecordingResult& result) {
        if (result.success) {
            printf("Video saved to: %s\n", result.file_path.c_str());
            printf("Duration: %lld ms\n", static_cast<long long>(result.duration_ms));
            printf("Size: %lld bytes\n", static_cast<long long>(result.file_size_bytes));
        } else {
            fprintf(stderr, "Recording failed: %s\n", result.error_message.c_str());
        }
    });

    // Initialize
    if (!g_recorder->initialize(recorder_config)) {
        fprintf(stderr, "Failed to initialize recorder\n");
        return 1;
    }

    // Start recording
    if (args.replay_mode) {
        printf("Starting replay mode (%d seconds)...\n", args.replay_duration);
        printf("Press Ctrl+C to stop, send SIGUSR1 to save replay\n");
        g_recorder->start_replay();

        // Wait for signal
        while (!g_should_exit.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        g_recorder->stop();
    } else {
        printf("Recording to %s...\n", args.output.c_str());
        printf("Press Ctrl+C to stop and save\n");

        if (args.record_while_replay) {
            // Replay + recording mode
            g_recorder->start_replay(args.output);
        } else {
            g_recorder->start_recording(args.output);
        }

        // Main loop
        while (!g_should_exit.load()) {
            // Print stats
            static int64_t last_print = 0;
            auto now = std::chrono::steady_clock::now();
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();

            if (now_ms - last_print > 2000) { // Print every 2 seconds
                last_print = now_ms;
                printf("\r  Recorded: %.1f sec | Frames: %lld | FPS: %d    ",
                       g_recorder->recording_duration_seconds(),
                       static_cast<long long>(g_recorder->frames_recorded()),
                       g_recorder->current_fps());
                fflush(stdout);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        printf("\n");
    }

    g_recorder.reset();
    printf("Done.\n");

    return 0;
}
