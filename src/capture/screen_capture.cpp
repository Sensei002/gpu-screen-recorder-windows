#include "capture/screen_capture.h"
#include "common/log.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <chrono>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#ifndef _XM_NO_INTRINSICS_
#include <DirectXMath.h>
#endif

// ─── Error checking helper ──────────────────────────────────────────────────
#define DX_CHECK(hr, msg) \
    do { \
        if (FAILED(hr)) { \
            LOG_ERROR(msg " (HRESULT: 0x%08X)", static_cast<unsigned int>(hr)); \
            return false; \
        } \
    } while(0)

// ─── Constructor / Destructor ───────────────────────────────────────────────

ScreenCapture::ScreenCapture() = default;

ScreenCapture::~ScreenCapture() {
    shutdown();
}

// ─── List monitors ──────────────────────────────────────────────────────────

std::vector<MonitorInfo> ScreenCapture::list_monitors() {
    std::vector<MonitorInfo> monitors;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return monitors;

    IDXGIAdapter1* adapter = nullptr;
    for (UINT adapter_idx = 0; factory->EnumAdapters1(adapter_idx, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapter_idx) {
        IDXGIOutput* output = nullptr;
        for (UINT output_idx = 0; adapter->EnumOutputs(output_idx, &output) != DXGI_ERROR_NOT_FOUND; ++output_idx) {
            DXGI_OUTPUT_DESC output_desc;
            if (SUCCEEDED(output->GetDesc(&output_desc))) {
                MonitorInfo info;
                char name[64];
                int name_len = WideCharToMultiByte(CP_UTF8, 0, output_desc.DeviceName, -1, name, sizeof(name), nullptr, nullptr);
                if (name_len > 0) {
                    info.name = std::string(name, name_len - 1);
                } else {
                    info.name = "DISPLAY" + std::to_string(monitors.size() + 1);
                }
                info.width = static_cast<int>(output_desc.DesktopCoordinates.right - output_desc.DesktopCoordinates.left);
                info.height = static_cast<int>(output_desc.DesktopCoordinates.bottom - output_desc.DesktopCoordinates.top);
                info.x = output_desc.DesktopCoordinates.left;
                info.y = output_desc.DesktopCoordinates.top;
                info.is_primary = (output_desc.AttachedToDesktop && output_desc.DesktopCoordinates.left == 0 && output_desc.DesktopCoordinates.top == 0);
                info.adapter_index = adapter_idx;
                info.output_index = output_idx;

                // Get refresh rate from the first supported mode
                UINT num_modes = 0;
                hr = output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &num_modes, nullptr);
                if (SUCCEEDED(hr) && num_modes > 0) {
                    std::vector<DXGI_MODE_DESC> modes(num_modes);
                    output->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &num_modes, modes.data());
                    for (const auto& mode : modes) {
                        if (mode.Width == static_cast<UINT>(info.width) && mode.Height == static_cast<UINT>(info.height)) {
                            if (mode.RefreshRate.Numerator > 0 && mode.RefreshRate.Denominator > 0) {
                                info.refresh_rate = static_cast<int>(mode.RefreshRate.Numerator / mode.RefreshRate.Denominator);
                            }
                            break;
                        }
                    }
                }

                monitors.push_back(info);
            }
            output->Release();
            output = nullptr;
        }
        adapter->Release();
        adapter = nullptr;
    }

    factory->Release();
    return monitors;
}

// ─── Initialize ─────────────────────────────────────────────────────────────

bool ScreenCapture::initialize(const CaptureConfig& config) {
    m_config = config;

    if (!init_d3d11()) {
        LOG_ERROR("Failed to initialize D3D11");
        return false;
    }

    if (!init_duplication()) {
        LOG_ERROR("Failed to initialize desktop duplication");
        return false;
    }

    LOG_INFO("Screen capture initialized: %dx%d @ %d fps",
             m_monitor_width, m_monitor_height, m_config.fps);
    return true;
}

void ScreenCapture::shutdown() {
    stop_capture();
    cleanup_duplication();

    if (m_cpu_access_texture) {
        m_cpu_access_texture->Release();
        m_cpu_access_texture = nullptr;
    }

    if (m_context) {
        m_context->Release();
        m_context = nullptr;
    }

    if (m_device) {
        m_device->Release();
        m_device = nullptr;
    }
}

bool ScreenCapture::init_d3d11() {
    // Create D3D11 device
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL selected_level;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Use default adapter
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,                    // Module
        flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &m_device,
        &selected_level,
        &m_context
    );

    DX_CHECK(hr, "Failed to create D3D11 device");

    if (selected_level < D3D_FEATURE_LEVEL_11_0) {
        LOG_ERROR("D3D11 feature level 11.0+ required, got %d", selected_level);
        return false;
    }

    LOG_INFO("D3D11 initialized with feature level %d.%d",
             (selected_level >> 12) & 0xf, (selected_level >> 8) & 0xf);

    // Get the DXGI device for later use
    IDXGIDevice* dxgi_device = nullptr;
    hr = m_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device));
    DX_CHECK(hr, "Failed to get DXGI device");

    IDXGIAdapter* dxgi_adapter = nullptr;
    hr = dxgi_device->GetAdapter(&dxgi_adapter);
    if (SUCCEEDED(hr)) {
        hr = dxgi_adapter->QueryInterface(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&m_adapter));
        dxgi_adapter->Release();
    }
    dxgi_device->Release();

    return true;
}

bool ScreenCapture::init_duplication() {
    if (!m_adapter) {
        LOG_ERROR("No adapter available");
        return false;
    }

    // Find the target monitor
    IDXGIOutput* target_output = nullptr;
    UINT target_output_idx = 0;
    bool found = false;

    for (UINT output_idx = 0; m_adapter->EnumOutputs(output_idx, &target_output) != DXGI_ERROR_NOT_FOUND; ++output_idx) {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(target_output->GetDesc(&desc))) {
            char name[64];
            int name_len = WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, name, sizeof(name), nullptr, nullptr);
            std::string monitor_name;
            if (name_len > 0) {
                monitor_name = std::string(name, name_len - 1);
            }

            bool matches = m_config.monitor_name.empty() || 
                          monitor_name == m_config.monitor_name ||
                          desc.DesktopCoordinates.left == 0; // Primary monitor fallback

            if (matches) {
                m_output = target_output;
                target_output_idx = output_idx;
                found = true;
                m_monitor_width = static_cast<int>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                m_monitor_height = static_cast<int>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                LOG_INFO("Selected monitor: %s (%dx%d at %d,%d)",
                         monitor_name.c_str(), m_monitor_width, m_monitor_height,
                         desc.DesktopCoordinates.left, desc.DesktopCoordinates.top);
                break;
            }
        }
        target_output->Release();
        target_output = nullptr;
    }

    if (!found) {
        LOG_ERROR("Could not find target monitor '%s'", m_config.monitor_name.c_str());
        return false;
    }

    // Get output duplication
    IDXGIOutput1* output1 = nullptr;
    HRESULT hr = m_output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    DX_CHECK(hr, "Failed to get IDXGIOutput1");

    hr = output1->DuplicateOutput(m_device, &m_duplication);
    output1->Release();

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create desktop duplication (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        LOG_ERROR("Make sure you're running on Windows 8+ with D3D11.1+ support");
        LOG_ERROR("Try: Ensure your GPU driver is up to date");
        return false;
    }

    // Create CPU-accessible staging texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_monitor_width;
    desc.Height = m_monitor_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    hr = m_device->CreateTexture2D(&desc, nullptr, &m_cpu_access_texture);
    DX_CHECK(hr, "Failed to create staging texture");

    return true;
}

void ScreenCapture::cleanup_duplication() {
    if (m_duplication) {
        m_duplication->Release();
        m_duplication = nullptr;
    }
    if (m_output) {
        m_output->Release();
        m_output = nullptr;
    }
    if (m_adapter) {
        m_adapter->Release();
        m_adapter = nullptr;
    }
}

// ─── Capture control ────────────────────────────────────────────────────────

bool ScreenCapture::start_capture(FrameCallback on_frame) {
    if (m_capturing.load()) {
        LOG_WARN("Capture already running");
        return false;
    }

    m_on_frame = std::move(on_frame);
    m_should_stop = false;
    m_capturing = true;
    m_frame_count = 0;
    m_current_fps = 0;
    m_fps_counter = 0;
    m_last_fps_time = 0;

    m_capture_thread = std::thread(&ScreenCapture::capture_thread_func, this);

    LOG_INFO("Screen capture started");
    return true;
}

void ScreenCapture::stop_capture() {
    if (!m_capturing.load()) return;

    m_should_stop = true;
    m_capturing = false;

    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }

    LOG_INFO("Screen capture stopped - captured %llu frames", 
             static_cast<unsigned long long>(m_frame_count.load()));
}

// ─── Capture thread ─────────────────────────────────────────────────────────

void ScreenCapture::capture_thread_func() {
    // Set thread priority to highest for consistent timing
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Set thread name for debugging
    typedef HRESULT(WINAPI* SetThreadDescriptionFunc)(HANDLE, PCWSTR);
    auto set_thread_desc = reinterpret_cast<SetThreadDescriptionFunc>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription"));
    if (set_thread_desc) {
        set_thread_desc(GetCurrentThread(), L"GSR_CaptureThread");
    }

    int64_t frame_interval_us = 1000000 / m_config.fps;
    auto next_frame_time = std::chrono::steady_clock::now();
    VideoFrame frame;

    while (!m_should_stop.load()) {
        if (m_config.capture_mode == FrameCaptureMode::FixedFPS) {
            next_frame_time += std::chrono::microseconds(frame_interval_us);
        }

        bool acquired = acquire_frame(frame);
        if (acquired) {
            // Timestamp
            auto now = std::chrono::steady_clock::now();
            frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
            frame.width = m_monitor_width;
            frame.height = m_monitor_height;
            frame.format = PixelFormat::BGRA;

            // Fire callback
            if (m_on_frame) {
                m_on_frame(frame);
            }

            m_frame_count++;
            m_fps_counter++;

            // FPS calculation
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            if (now_ms - m_last_fps_time >= 1000) {
                m_current_fps = m_fps_counter;
                m_fps_counter = 0;
                m_last_fps_time = now_ms;
            }

            release_frame();
        } else {
            // Frame not ready yet, small sleep
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        if (m_config.capture_mode == FrameCaptureMode::FixedFPS) {
            std::this_thread::sleep_until(next_frame_time);
        }
    }
}

bool ScreenCapture::acquire_frame(VideoFrame& frame) {
    if (!m_duplication) return false;

    IDXGIResource* desktop_resource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frame_info;

    HRESULT hr = m_duplication->AcquireNextFrame(0, &frame_info, &desktop_resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }

    if (hr == DXGI_ERROR_ACCESS_LOST) {
        LOG_WARN("Desktop duplication access lost, reinitializing...");
        cleanup_duplication();
        if (!init_duplication()) {
            LOG_ERROR("Failed to reinitialize desktop duplication");
        }
        return false;
    }

    if (FAILED(hr) || !desktop_resource) {
        return false;
    }

    // Get the desktop texture
    ID3D11Texture2D* desktop_texture = nullptr;
    hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), 
                                          reinterpret_cast<void**>(&desktop_texture));
    desktop_resource->Release();

    if (FAILED(hr) || !desktop_texture) {
        return false;
    }

    // Copy to CPU-accessible texture
    m_context->CopyResource(m_cpu_access_texture, desktop_texture);
    desktop_texture->Release();

    // Map for CPU read and copy frame data to persistent buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m_context->Map(m_cpu_access_texture, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    // Allocate persistent buffer for frame data
    size_t data_size = static_cast<size_t>(m_monitor_height) * mapped.RowPitch;
    if (m_frame_buffer.size() < data_size) {
        m_frame_buffer.resize(data_size);
    }
    memcpy(m_frame_buffer.data(), mapped.pData, data_size);

    frame.data = m_frame_buffer.data();
    frame.stride = static_cast<int>(mapped.RowPitch);

    m_context->Unmap(m_cpu_access_texture, 0);

    return true;
}

void ScreenCapture::release_frame() {
    // Unmap is now done immediately in acquire_frame after copying data.
    // Only release the duplication frame.
    if (m_duplication) {
        m_duplication->ReleaseFrame();
    }
}

bool ScreenCapture::capture_single_frame(VideoFrame& out_frame) {
    if (!m_duplication) return false;

    bool acquired = acquire_frame(out_frame);
    if (acquired) {
        auto now = std::chrono::steady_clock::now();
        out_frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            now.time_since_epoch()).count();
        out_frame.width = m_monitor_width;
        out_frame.height = m_monitor_height;
        out_frame.format = PixelFormat::BGRA;
        return true;
    }

    return false;
}
