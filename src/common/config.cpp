#include "common/config.h"
#include "common/log.h"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#endif

Config& Config::instance() {
    static Config config;
    return config;
}

std::string Config::default_config_dir() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::string(path) + "\\gsr";
    }
    return "C:\\gsr-config";
#else
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/.config/gsr";
    return "/tmp/gsr-config";
#endif
}

std::string Config::default_videos_dir() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYVIDEO, nullptr, 0, path))) {
        return std::string(path) + "\\GPU Screen Recorder";
    }
    return "C:\\Videos\\GPU Screen Recorder";
#else
    const char* home = getenv("HOME");
    if (home) return std::string(home) + "/Videos";
    return "/tmp/Videos";
#endif
}

void Config::set_defaults() {
    m_recorder.output_mode = OutputMode::File;
    m_recorder.output_path = default_videos_dir() + "\\recording.mp4";
    m_recorder.output_directory = default_videos_dir();

    m_recorder.capture.width = 0;
    m_recorder.capture.height = 0;
    m_recorder.capture.fps = 60;
    m_recorder.capture.capture_mode = FrameCaptureMode::FixedFPS;
    m_recorder.capture.capture_cursor = true;
    m_recorder.capture.capture_audio = true;

    m_recorder.video.codec = VideoCodec::H264;
    m_recorder.video.width = 0;
    m_recorder.video.height = 0;
    m_recorder.video.fps = 60;
    m_recorder.video.bitrate_kbps = 20000;
    m_recorder.video.constant_quality = true;
    m_recorder.video.quality = 23;
    m_recorder.video.enable_hdr = false;
    m_recorder.video.keyint_seconds = 2;

    m_recorder.replay_duration_seconds = 30;
    m_recorder.replay_storage = ReplayStorage::RAM;

    m_screenshot.format = ImageFormat::PNG;
    m_screenshot.quality = 95;
    m_screenshot.output_path = default_videos_dir();

    m_ui.start_on_boot = false;
    m_ui.minimize_to_tray = true;
    m_ui.show_notifications = true;
    m_ui.start_minimized = false;
    m_ui.overlay_opacity = 90;
    m_ui.overlay_scale = 1.0f;
    m_ui.theme = "dark";
}

bool Config::load(const std::string& path) {
    set_defaults();

    std::string config_path = path.empty() ? default_config_dir() + "\\gsr.conf" : path;
    m_config_path = config_path;

    // Ensure directory exists
    std::filesystem::path dir = std::filesystem::path(config_path).parent_path();
    std::filesystem::create_directories(dir);

    // Ensure videos directory exists
    std::filesystem::create_directories(default_videos_dir());

    // Logs directory
    m_logs_path = std::filesystem::path(config_path).parent_path().string() + "\\logs";
    std::filesystem::create_directories(m_logs_path);

    std::ifstream file(config_path);
    if (!file.is_open()) {
        LOG_INFO("No config file found at %s, using defaults", config_path.c_str());
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        parse_line(line);
    }

    LOG_INFO("Loaded config from %s", config_path.c_str());
    return true;
}

bool Config::parse_line(const std::string& line) {
    // Trim whitespace
    std::string trimmed = line;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

    if (trimmed.empty() || trimmed[0] == '#') return true;

    auto eq = trimmed.find('=');
    if (eq == std::string::npos) return false;

    std::string key = trimmed.substr(0, eq);
    std::string value = trimmed.substr(eq + 1);

    // Trim key/value
    key.erase(0, key.find_first_not_of(" \t"));
    key.erase(key.find_last_not_of(" \t") + 1);
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);

    // Remove surrounding quotes
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }

    try {
        if (key == "video_codec") {
            if (value == "h264") m_recorder.video.codec = VideoCodec::H264;
            else if (value == "hevc") m_recorder.video.codec = VideoCodec::HEVC;
            else if (value == "av1") m_recorder.video.codec = VideoCodec::AV1;
            else if (value == "vp8") m_recorder.video.codec = VideoCodec::VP8;
            else if (value == "vp9") m_recorder.video.codec = VideoCodec::VP9;
        } else if (key == "video_fps") {
            m_recorder.video.fps = std::stoi(value);
        } else if (key == "video_bitrate") {
            m_recorder.video.bitrate_kbps = std::stoi(value);
        } else if (key == "video_quality") {
            m_recorder.video.quality = std::stoi(value);
        } else if (key == "video_constant_quality") {
            m_recorder.video.constant_quality = (value == "true" || value == "yes" || value == "1");
        } else if (key == "capture_fps") {
            m_recorder.capture.fps = std::stoi(value);
        } else if (key == "capture_cursor") {
            m_recorder.capture.capture_cursor = (value == "true" || value == "yes" || value == "1");
        } else if (key == "capture_audio") {
            m_recorder.capture.capture_audio = (value == "true" || value == "yes" || value == "1");
        } else if (key == "replay_duration") {
            m_recorder.replay_duration_seconds = std::stoi(value);
        } else if (key == "replay_storage") {
            m_recorder.replay_storage = (value == "disk") ? ReplayStorage::Disk : ReplayStorage::RAM;
        } else if (key == "audio_codec") {
            if (value == "aac") m_recorder.capture.audio_config.codec = AudioCodec::AAC;
            else if (value == "opus") m_recorder.capture.audio_config.codec = AudioCodec::Opus;
        } else if (key == "audio_bitrate") {
            m_recorder.capture.audio_config.bitrate_kbps = std::stoi(value);
        } else if (key == "output_dir") {
            m_recorder.output_directory = value;
        } else if (key == "overlay_opacity") {
            m_ui.overlay_opacity = std::stoi(value);
        } else if (key == "overlay_scale") {
            m_ui.overlay_scale = std::stof(value);
        } else if (key == "theme") {
            m_ui.theme = value;
        } else if (key == "minimize_to_tray") {
            m_ui.minimize_to_tray = (value == "true" || value == "yes" || value == "1");
        } else if (key == "show_notifications") {
            m_ui.show_notifications = (value == "true" || value == "yes" || value == "1");
        }
    } catch (const std::exception& e) {
        LOG_WARN("Failed to parse config key '%s': %s", key.c_str(), e.what());
        return false;
    }

    return true;
}

std::string Config::serialize() const {
    std::ostringstream ss;
    ss << "# GPU Screen Recorder Configuration\n";
    ss << "# Auto-generated configuration file\n\n";

    ss << "# Video settings\n";
    ss << "video_codec = " << videoCodecToString(m_recorder.video.codec) << "\n";
    ss << "video_fps = " << m_recorder.video.fps << "\n";
    ss << "video_bitrate = " << m_recorder.video.bitrate_kbps << "\n";
    ss << "video_quality = " << m_recorder.video.quality << "\n";
    ss << "video_constant_quality = " << (m_recorder.video.constant_quality ? "true" : "false") << "\n\n";

    ss << "# Capture settings\n";
    ss << "capture_fps = " << m_recorder.capture.fps << "\n";
    ss << "capture_cursor = " << (m_recorder.capture.capture_cursor ? "true" : "false") << "\n";
    ss << "capture_audio = " << (m_recorder.capture.capture_audio ? "true" : "false") << "\n\n";

    ss << "# Audio settings\n";
    ss << "audio_codec = " << audioCodecToString(m_recorder.capture.audio_config.codec) << "\n";
    ss << "audio_bitrate = " << m_recorder.capture.audio_config.bitrate_kbps << "\n\n";

    ss << "# Replay settings\n";
    ss << "replay_duration = " << m_recorder.replay_duration_seconds << "\n";
    ss << "replay_storage = " << (m_recorder.replay_storage == ReplayStorage::RAM ? "ram" : "disk") << "\n\n";

    ss << "# Paths\n";
    ss << "output_dir = " << m_recorder.output_directory << "\n\n";

    ss << "# UI settings\n";
    ss << "overlay_opacity = " << m_ui.overlay_opacity << "\n";
    ss << "overlay_scale = " << m_ui.overlay_scale << "\n";
    ss << "theme = " << m_ui.theme << "\n";
    ss << "minimize_to_tray = " << (m_ui.minimize_to_tray ? "true" : "false") << "\n";
    ss << "show_notifications = " << (m_ui.show_notifications ? "true" : "false") << "\n";

    return ss.str();
}

bool Config::save(const std::string& path) {
    std::string config_path = path.empty() ? m_config_path : path;
    if (config_path.empty()) {
        config_path = default_config_dir() + "\\gsr.conf";
    }

    std::filesystem::path dir = std::filesystem::path(config_path).parent_path();
    std::filesystem::create_directories(dir);

    std::ofstream file(config_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to write config to %s", config_path.c_str());
        return false;
    }

    file << serialize();
    file.close();

    LOG_INFO("Saved config to %s", config_path.c_str());
    return true;
}
