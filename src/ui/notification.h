#pragma once

#include <windows.h>

#include "common/types.h"
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <chrono>
#include <queue>
#include <mutex>
#include <thread>
#include <cstdint>

// ─── Notification Types ─────────────────────────────────────────────────────

enum class NotificationType {
    RecordingStarted,
    RecordingStopped,
    ReplaySaved,
    ScreenshotTaken,
    StreamingStarted,
    StreamingStopped,
    Error,
    Info,
    Warning,
};

struct Notification {
    NotificationType type = NotificationType::Info;
    std::string title;
    std::string message;
    std::string subtitle;      // Additional info (e.g., file path, duration)
    std::string icon;           // Icon identifier
    int duration_ms = 3500;    // How long to show
    int priority = 0;          // Higher priority interrupts current notification

    // Helper to create standard notifications
    static Notification recording_started(const std::string& path = "");
    static Notification recording_stopped(const std::string& path, double duration_sec, int64_t file_size);
    static Notification replay_saved(const std::string& path, int duration_sec);
    static Notification screenshot_taken(const std::string& path, const std::string& format);
    static Notification streaming_started(const std::string& url);
    static Notification streaming_stopped(double duration_sec);
    static Notification error(const std::string& message);
    static Notification info(const std::string& title, const std::string& message);
};

// ─── Notification Overlay ───────────────────────────────────────────────────

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;

class NotificationRenderer {
public:
    NotificationRenderer();
    ~NotificationRenderer();

    // Initialize renderer
    bool initialize();
    void shutdown();

    // Show a notification on screen (non-blocking, queues it)
    void show_notification(const Notification& notification);

    // Get current notification state
    bool is_showing() const { return m_showing.load(); }
    bool has_pending() const;

    // Run the render loop (blocking)
    void run();
    void stop();

    // Window handle
    HWND window() const { return m_hwnd; }

private:
    bool create_window();
    bool create_d3d11();
    bool create_render_target();
    void cleanup_render_target();
    void render_notification(const Notification& notification, float alpha);
    void draw_background(int window_w, int window_h, float alpha);
    void draw_text(const std::string& title, const std::string& message,
                   const std::string& subtitle, NotificationType type,
                   int window_w, int window_h, float alpha);
    void draw_icon(NotificationType type, float alpha);
    void update_window_position();

    // Window
    HWND m_hwnd = nullptr;
    WNDCLASSEX m_wc = {};
    int m_window_width = 360;
    int m_window_height = 80;

    // Monitor position (show on cursor's monitor)
    int m_monitor_x = 0;
    int m_monitor_y = 0;
    int m_monitor_width = 0;
    int m_monitor_height = 0;

    // DirectX
    ID3D11Device* m_d3d_device = nullptr;
    ID3D11DeviceContext* m_d3d_context = nullptr;
    IDXGISwapChain* m_swap_chain = nullptr;
    ID3D11RenderTargetView* m_render_target = nullptr;

    // Font rendering (using GDI fallback via texture atlas)
    HFONT m_font_title = nullptr;
    HFONT m_font_message = nullptr;
    HFONT m_font_subtitle = nullptr;

    // Notification state
    std::queue<Notification> m_pending_notifications;
    std::mutex m_queue_mutex;

    std::atomic<bool> m_showing{false};
    std::atomic<bool> m_running{false};

    // Current notification animation
    struct ActiveNotification {
        Notification notification;
        std::chrono::steady_clock::time_point start_time;
        float current_alpha = 0.0f;
        int slide_offset = 0;
    };

    std::unique_ptr<ActiveNotification> m_active;

    // Window positioning
    bool m_window_positioned = false;

    // Static window procedure
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};

// ─── Notification Manager (IPC Controller) ──────────────────────────────────

class NotificationManager {
public:
    NotificationManager();
    ~NotificationManager();

    bool initialize();
    void shutdown();

    // Show a notification
    void show(const Notification& notification);

    // Connect to an existing notification renderer (when running as separate process)
    bool connect_to_pipe(const std::string& pipe_name);

    // Send notification remotely via named pipe (for gsr-ui-cli)
    bool send_remote_notification(const Notification& notification);

    // Set a local renderer
    void set_renderer(std::shared_ptr<NotificationRenderer> renderer) { m_renderer = renderer; }

private:
    std::shared_ptr<NotificationRenderer> m_renderer;
    std::string m_pipe_name;
    bool m_use_pipe = false;
};
