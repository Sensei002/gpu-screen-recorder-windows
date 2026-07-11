#pragma once

#include "common/types.h"
#include <memory>
#include <functional>
#include <vector>
#include <atomic>
#include <thread>

struct IDXGIAdapter1;
struct IDXGIOutput;
struct IDXGIOutputDuplication;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    // Prevent copy
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Initialize DXGI capture
    bool initialize(const CaptureConfig& config);
    void shutdown();

    // List available monitors
    static std::vector<MonitorInfo> list_monitors();

    // Start/stop capture thread
    bool start_capture(FrameCallback on_frame);
    void stop_capture();

    // Take a single screenshot
    bool capture_single_frame(VideoFrame& out_frame);

    // Get current capture info
    bool is_capturing() const { return m_capturing.load(); }
    int capture_fps() const { return m_current_fps; }
    uint64_t captured_frames() const { return m_frame_count; }

    // Get the D3D11 device (for sharing with encoder)
    ID3D11Device* d3d_device() const { return m_device; }
    ID3D11DeviceContext* d3d_context() const { return m_context; }

private:
    bool init_d3d11();
    bool init_duplication();
    void capture_thread_func();
    bool acquire_frame(VideoFrame& frame);
    void release_frame();
    bool copy_texture(ID3D11Texture2D* src, ID3D11Texture2D** dst);
    void cleanup_duplication();

    // D3D11
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGIAdapter1* m_adapter = nullptr;
    IDXGIOutput* m_output = nullptr;
    IDXGIOutputDuplication* m_duplication = nullptr;

    // Textures
    ID3D11Texture2D* m_cpu_access_texture = nullptr;

    // Persistent frame buffer (avoids use-after-free from mapped texture memory)
    std::vector<uint8_t> m_frame_buffer;

    // Config
    CaptureConfig m_config;
    int m_monitor_width = 0;
    int m_monitor_height = 0;

    // Threading
    std::thread m_capture_thread;
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_should_stop{false};

    // Statistics
    std::atomic<int> m_current_fps{0};
    std::atomic<uint64_t> m_frame_count{0};
    int64_t m_last_fps_time = 0;
    int m_fps_counter = 0;

    // Callback
    FrameCallback m_on_frame;

    // Cursor
    bool m_cursor_visible = true;
    int m_cursor_x = 0;
    int m_cursor_y = 0;
};
