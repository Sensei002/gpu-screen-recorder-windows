// GPU Screen Recorder UI - Windows Overlay Application
// A ShadowPlay-style fullscreen overlay UI for GPU Screen Recorder
// Usage: gsr-ui

#include "common/log.h"
#include "common/config.h"
#include "common/types.h"
#include "recorder/recorder.h"
#include "ui/overlay.h"
#include "ui/hotkeys.h"
#include "ui/notification.h"

#include <windows.h>
#include <cstdio>
#include <memory>
#include <atomic>
#include <thread>

// ─── Global State ───────────────────────────────────────────────────────────

static std::unique_ptr<Recorder> g_recorder;
static std::unique_ptr<Overlay> g_overlay;
static std::unique_ptr<HotkeyManager> g_hotkeys;
static std::shared_ptr<NotificationManager> g_notification_mgr;
static std::shared_ptr<NotificationRenderer> g_notification_renderer;
static std::atomic<bool> g_running{true};

// ─── Forward Declarations ───────────────────────────────────────────────────

void setup_callbacks();

// ─── Application Entry Point ────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    // Initialize logger
    Logger::instance().set_level(LogLevel::Info);
    Logger::instance().set_file_output(Config::default_config_dir() + "\\gsr-ui.log");

    LOG_INFO("═══════════════════════════════════════════");
    LOG_INFO(" GPU Screen Recorder UI - Windows Edition");
    LOG_INFO(" Version 1.0.0");
    LOG_INFO("═══════════════════════════════════════════");

    // Load configuration
    Config::instance().load();
    auto& cfg = Config::instance();

    // Create recorder
    g_recorder = std::make_unique<Recorder>();
    if (!g_recorder->initialize(cfg.recorder())) {
        LOG_ERROR("Failed to initialize recorder");
        MessageBoxA(nullptr, "Failed to initialize recorder.\n"
                    "Make sure your GPU drivers are up to date and you have DirectX 11 support.",
                    "GPU Screen Recorder", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Make set_notification_manager accessible
    g_recorder->set_notification_manager(g_notification_mgr);

    // Create overlay
    g_overlay = std::make_unique<Overlay>();
    if (!g_overlay->initialize(hInstance, nCmdShow)) {
        LOG_ERROR("Failed to initialize overlay");
        return 1;
    }

    // Create notification system
    g_notification_mgr = std::make_shared<NotificationManager>();
    g_notification_mgr->initialize();
    
    // Create notification renderer in background thread
    g_notification_renderer = std::make_shared<NotificationRenderer>();
    if (g_notification_renderer->initialize()) {
        g_notification_mgr->set_renderer(g_notification_renderer);
        
        // Run notification renderer on its own thread
        std::thread notif_thread([&]() {
            g_notification_renderer->run();
        });
        notif_thread.detach();
        
        LOG_INFO("Notification system initialized");
    } else {
        LOG_WARN("Failed to initialize notification system");
        g_notification_renderer.reset();
    }

    // Create hotkey manager
    g_hotkeys = std::make_unique<HotkeyManager>();
    if (!g_hotkeys->initialize()) {
        LOG_WARN("Failed to initialize hotkey manager");
    }

    // Set up all callbacks
    setup_callbacks();

    // Start replay mode by default
    if (cfg.recorder().output_mode == OutputMode::Replay ||
        cfg.recorder().output_mode == OutputMode::ReplayAndRecord) {
        LOG_INFO("Starting replay mode by default");
        g_recorder->start_replay(cfg.recorder().output_directory);
    }

    // Start minimized if configured
    if (cfg.ui().start_minimized) {
        LOG_INFO("Starting minimized to tray");
        g_overlay->hide();
    }

    // Initialize hotkey message loop in background thread
    std::thread hotkey_thread([&]() {
        g_hotkeys->run_message_loop();
    });

    LOG_INFO("GPU Screen Recorder UI is running");
    LOG_INFO("Press Alt+Z to show/hide the overlay");

    // Run overlay message loop (main thread)
    if (cfg.ui().start_minimized) {
        g_overlay->hide();
    } else {
        g_overlay->show();
    }

    g_overlay->run();

    // Cleanup
    g_running = false;
    g_hotkeys->stop_message_loop();
    g_recorder->stop();

    if (hotkey_thread.joinable()) {
        hotkey_thread.join();
    }

    g_hotkeys.reset();
    g_overlay.reset();
    g_recorder.reset();

    LOG_INFO("GPU Screen Recorder UI shutdown complete");
    return 0;
}

// ─── Callbacks ──────────────────────────────────────────────────────────────

void setup_callbacks() {
    // ─── Overlay callbacks ───────────────────────────────────────────────

    g_overlay->on_start_recording = []() {
        LOG_INFO("UI: Start recording requested");
        auto& recorder = Config::instance().recorder();
        g_recorder->start_recording(recorder.output_path);
        g_overlay->hide(); // Hide overlay when recording starts (like ShadowPlay)
    };

    g_overlay->on_stop_recording = []() {
        LOG_INFO("UI: Stop recording requested");
        g_recorder->stop();
        g_overlay->show(); // Show overlay again
    };

    g_overlay->on_save_replay = []() {
        LOG_INFO("UI: Save replay requested");
        g_recorder->save_replay();
    };

    g_overlay->on_open_settings = []() {
        LOG_INFO("UI: Settings opened");
    };

    g_overlay->on_quit = []() {
        LOG_INFO("UI: Quit requested");
        g_overlay->stop();
    };

    g_overlay->on_take_screenshot = []() {
        LOG_INFO("UI: Screenshot requested");
        g_recorder->save_screenshot();
    };

    // ─── Hotkey callbacks ───────────────────────────────────────────────

    g_hotkeys->set_on_toggle_ui([&]() {
        LOG_DEBUG("Hotkey: Toggle UI");
        g_overlay->toggle();
    });

    g_hotkeys->set_on_save_replay([&]() {
        LOG_DEBUG("Hotkey: Save Replay");
        if (g_recorder->is_replay_active()) {
            g_recorder->save_replay();
        }
    });

    g_hotkeys->set_on_toggle_recording([&]() {
        LOG_DEBUG("Hotkey: Toggle Recording");
        if (g_recorder->is_recording()) {
            g_recorder->stop();
            g_overlay->show();
        } else if (g_recorder->is_replay_active()) {
            auto& cfg = Config::instance().recorder();
            g_recorder->start_recording(cfg.output_path);
            g_overlay->hide();
        } else {
            auto& cfg = Config::instance().recorder();
            g_recorder->start_recording(cfg.output_path);
            g_overlay->hide();
        }
    });

    g_hotkeys->set_on_take_screenshot([&]() {
        LOG_DEBUG("Hotkey: Screenshot");
        g_recorder->save_screenshot();
    });

    g_hotkeys->set_on_show_overlay([&]() {
        g_overlay->show();
    });

    g_hotkeys->set_on_hide_overlay([&]() {
        g_overlay->hide();
    });

    // ─── Periodic data update ───────────────────────────────────────────
    std::thread data_thread([]() {
        while (g_running.load()) {
            if (g_overlay) {
                Overlay::OverlayData data;
                data.recording_duration = g_recorder->recording_duration_seconds();
                data.fps = g_recorder->current_fps();
                data.frames_recorded = g_recorder->frames_recorded();
                data.file_size = g_recorder->recording_file_size();
                data.replay_mode = g_recorder->is_replay_active();
                data.replay_duration = Config::instance().recorder().replay_duration_seconds;
                data.output_path = Config::instance().recorder().output_path;
                data.encoder_name = "auto"; // Would come from video encoder

                g_overlay->update_overlay_data(data);
                g_overlay->set_recorder_state(g_recorder->state());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });
    data_thread.detach();
}

// ─── System Tray ────────────────────────────────────────────────────────────

void setup_system_tray(HWND hwnd) {
    // Placeholder for system tray integration
    // In a full implementation, add NOTIFYICONDATA for system tray icon
    // with context menu: Show/Hide, Start/Stop Recording, Save Replay, Quit
    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    strncpy_s(nid.szTip, "GPU Screen Recorder", sizeof(nid.szTip));
    Shell_NotifyIcon(NIM_ADD, &nid);
}
