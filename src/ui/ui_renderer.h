#pragma once

#include "common/types.h"
#include <memory>
#include <functional>

class Overlay;

class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    bool initialize(Overlay* overlay);
    void shutdown();
    void render_frame();

private:
    void render_main_window();
    void render_recording_panel();
    void render_replay_panel();
    void render_settings_panel();
    void render_stats_overlay();
    void render_settings_page();
    void render_output_tab();
    void render_video_tab();
    void render_audio_tab();
    void render_hotkeys_tab();
    void render_about_tab();
    void apply_theme();

    Overlay* m_overlay = nullptr;

    // UI state
    enum class Page {
        Main,
        Recording,
        Replay,
        Settings,
        About,
    };

    Page m_current_page = Page::Main;
    int m_settings_tab = 0; // 0=output, 1=video, 2=audio, 3=hotkeys, 4=about
    bool m_show_stats_overlay = true;

    // Recording state (cached)
    bool m_is_recording = false;
    bool m_is_replay_mode = false;

    // Style
    float m_scale = 1.0f;
};
