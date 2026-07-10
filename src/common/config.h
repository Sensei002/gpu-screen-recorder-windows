#pragma once

#include "common/types.h"
#include <string>
#include <fstream>
#include <filesystem>

class Config {
public:
    static Config& instance();

    bool load(const std::string& path = "");
    bool save(const std::string& path = "");

    // Accessors
    const RecorderConfig& recorder() const { return m_recorder; }
    RecorderConfig& recorder() { return m_recorder; }

    const ScreenshotConfig& screenshot() const { return m_screenshot; }
    ScreenshotConfig& screenshot() { return m_screenshot; }

    // Hotkeys
    struct HotkeyConfig {
        uint32_t toggle_ui = 0xAF;       // VK_LMENU (Alt) + 'Z' - typically set at runtime
        uint32_t toggle_ui_mod = 'Z';
        uint32_t save_replay = 0;         // Not set by default
        uint32_t save_replay_mod = 0;
        uint32_t toggle_recording = 0;
        uint32_t toggle_recording_mod = 0;
        uint32_t take_screenshot = 0;
        uint32_t take_screenshot_mod = 0;
        bool enable_hotkeys = true;
    };

    const HotkeyConfig& hotkeys() const { return m_hotkeys; }
    HotkeyConfig& hotkeys() { return m_hotkeys; }

    // UI
    struct UIConfig {
        bool start_on_boot = false;
        bool minimize_to_tray = true;
        bool show_notifications = true;
        bool start_minimized = false;

        // Overlay
        int overlay_opacity = 90; // 0-100
        float overlay_scale = 1.0f;
        std::string theme = "dark"; // dark, light, custom
    };

    const UIConfig& ui() const { return m_ui; }
    UIConfig& ui() { return m_ui; }

    // Paths
    std::string config_path() const { return m_config_path; }
    std::string logs_path() const { return m_logs_path; }

    // Default paths
    static std::string default_config_dir();
    static std::string default_videos_dir();

private:
    Config() = default;

    void set_defaults();
    bool parse_line(const std::string& line);
    std::string serialize() const;

    RecorderConfig m_recorder;
    ScreenshotConfig m_screenshot;
    HotkeyConfig m_hotkeys;
    UIConfig m_ui;
    std::string m_config_path;
    std::string m_logs_path;
};
