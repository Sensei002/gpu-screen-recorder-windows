#pragma once

#include "common/types.h"
#include <memory>
#include <atomic>
#include <functional>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

class UIRenderer;

class Overlay {
public:
    Overlay();
    ~Overlay();

    // Initialize the overlay window and DirectX
    bool initialize(HINSTANCE instance, int nCmdShow);
    void shutdown();

    // Show/hide/toggle overlay
    void show();
    void hide();
    void toggle();
    bool is_visible() const { return m_visible.load(); }

    // Set overlay position and size
    void set_position(int x, int y);
    void set_size(int width, int height);

    // Set the window to be transparent/click-through in certain modes
    void set_click_through(bool enabled);

    // Create the overlay window as a layered window that floats over everything
    HWND window() const { return m_hwnd; }

    // Run the message loop (blocking)
    void run();
    void stop();

    // Getters for ImGui
    ID3D11Device* d3d_device() const { return m_d3d_device; }
    ID3D11DeviceContext* d3d_context() const { return m_d3d_context; }
    IDXGISwapChain* swap_chain() const { return m_swap_chain; }
    HWND hwnd() const { return m_hwnd; }

    // Overlay state
    void set_recorder_state(RecordingState state) { m_recorder_state = state; }
    RecordingState recorder_state() const { return m_recorder_state; }

    // Dynamic data from recorder
    struct OverlayData {
        double recording_duration = 0;
        int fps = 0;
        int64_t frames_recorded = 0;
        int64_t file_size = 0;
        bool replay_mode = false;
        int replay_duration = 30;
        std::string output_path;
        std::string encoder_name;
    };

    void update_overlay_data(const OverlayData& data) { m_data = data; }
    const OverlayData& overlay_data() const { return m_data; }

    // Callbacks from UI
    std::function<void()> on_start_recording;
    std::function<void()> on_stop_recording;
    std::function<void()> on_save_replay;
    std::function<void()> on_open_settings;
    std::function<void()> on_quit;
    std::function<void()> on_take_screenshot;

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool create_window(HINSTANCE instance);
    bool create_d3d11();
    bool create_render_target();
    void cleanup_render_target();

    // Window
    HWND m_hwnd = nullptr;
    WNDCLASSEX m_wc = {};
    int m_width = 400;
    int m_height = 500;
    int m_pos_x = 100;
    int m_pos_y = 100;

    // DirectX
    ID3D11Device* m_d3d_device = nullptr;
    ID3D11DeviceContext* m_d3d_context = nullptr;
    IDXGISwapChain* m_swap_chain = nullptr;
    ID3D11RenderTargetView* m_render_target = nullptr;

    // UI Renderer
    std::unique_ptr<UIRenderer> m_ui_renderer;

    // State
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_running{false};
    std::atomic<RecordingState> m_recorder_state{RecordingState::Idle};
    OverlayData m_data;

    // Click-through
    bool m_click_through = false;
};
