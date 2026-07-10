#include "ui/notification.h"
#include "common/log.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ─── Notification Factory Methods ───────────────────────────────────────────

Notification Notification::recording_started(const std::string& path) {
    Notification n;
    n.type = NotificationType::RecordingStarted;
    n.title = "Recording Started";
    n.message = path.empty() ? "Recording in progress..." : "Saving to " + path;
    n.icon = "rec";
    return n;
}

Notification Notification::recording_stopped(const std::string& path, double duration_sec, int64_t file_size) {
    Notification n;
    n.type = NotificationType::RecordingStopped;
    n.title = "Recording Saved";
    
    char buf[256];
    int mins = static_cast<int>(duration_sec) / 60;
    int secs = static_cast<int>(duration_sec) % 60;
    
    const char* size_str = "0 B";
    char size_buf[32];
    if (file_size > 1073741824) {
        snprintf(size_buf, sizeof(size_buf), "%.1f GB", file_size / 1073741824.0);
    } else if (file_size > 1048576) {
        snprintf(size_buf, sizeof(size_buf), "%.1f MB", file_size / 1048576.0);
    } else if (file_size > 1024) {
        snprintf(size_buf, sizeof(size_buf), "%.1f KB", file_size / 1024.0);
    } else {
        snprintf(size_buf, sizeof(size_buf), "%lld B", static_cast<long long>(file_size));
    }
    size_str = size_buf;

    snprintf(buf, sizeof(buf), "%02d:%02d  |  %s", mins, secs, size_str);
    n.message = buf;
    n.subtitle = path;
    n.icon = "stop";
    n.duration_ms = 4000;
    return n;
}

Notification Notification::replay_saved(const std::string& path, int duration_sec) {
    Notification n;
    n.type = NotificationType::ReplaySaved;
    n.title = "Replay Saved";
    
    char buf[128];
    snprintf(buf, sizeof(buf), "Last %d seconds saved", duration_sec);
    n.message = buf;
    n.subtitle = path;
    n.icon = "replay";
    n.duration_ms = 4000;
    return n;
}

Notification Notification::screenshot_taken(const std::string& path, const std::string& format) {
    Notification n;
    n.type = NotificationType::ScreenshotTaken;
    n.title = "Screenshot Taken";
    n.message = format.empty() ? "Screenshot saved" : "Saved as " + format;
    n.subtitle = path;
    n.icon = "camera";
    n.duration_ms = 3000;
    return n;
}

Notification Notification::streaming_started(const std::string& url) {
    Notification n;
    n.type = NotificationType::StreamingStarted;
    n.title = "Streaming Started";
    n.message = url.empty() ? "Streaming to..." : url;
    n.icon = "stream";
    return n;
}

Notification Notification::streaming_stopped(double duration_sec) {
    Notification n;
    n.type = NotificationType::StreamingStopped;
    n.title = "Streaming Stopped";

    char buf[64];
    int mins = static_cast<int>(duration_sec) / 60;
    int secs = static_cast<int>(duration_sec) % 60;
    snprintf(buf, sizeof(buf), "Duration: %02d:%02d", mins, secs);
    n.message = buf;
    n.icon = "stop";
    n.duration_ms = 3500;
    return n;
}

Notification Notification::error(const std::string& message) {
    Notification n;
    n.type = NotificationType::Error;
    n.title = "Error";
    n.message = message;
    n.icon = "error";
    n.duration_ms = 5000;
    n.priority = 1;
    return n;
}

Notification Notification::info(const std::string& title, const std::string& message) {
    Notification n;
    n.type = NotificationType::Info;
    n.title = title;
    n.message = message;
    n.icon = "info";
    n.duration_ms = 3000;
    return n;
}

// ─── Notification Renderer ──────────────────────────────────────────────────

NotificationRenderer::NotificationRenderer() = default;

NotificationRenderer::~NotificationRenderer() {
    shutdown();
}

bool NotificationRenderer::initialize() {
    if (!create_window()) {
        LOG_ERROR("Failed to create notification overlay window");
        return false;
    }

    if (!create_d3d11()) {
        LOG_ERROR("Failed to create D3D11 for notification overlay");
        return false;
    }

    if (!create_render_target()) {
        LOG_ERROR("Failed to create render target for notification overlay");
        return false;
    }

    // Create GDI fonts for text rendering fallback
    m_font_title = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    m_font_message = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    m_font_subtitle = CreateFontA(11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    LOG_INFO("Notification renderer initialized");
    return true;
}

void NotificationRenderer::shutdown() {
    stop();

    if (m_font_title) DeleteObject(m_font_title);
    if (m_font_message) DeleteObject(m_font_message);
    if (m_font_subtitle) DeleteObject(m_font_subtitle);

    cleanup_render_target();

    if (m_swap_chain) { m_swap_chain->Release(); m_swap_chain = nullptr; }
    if (m_d3d_context) { m_d3d_context->Release(); m_d3d_context = nullptr; }
    if (m_d3d_device) { m_d3d_device->Release(); m_d3d_device = nullptr; }

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool NotificationRenderer::create_window() {
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GSRNotificationClass";

    if (!RegisterClassEx(&wc)) {
        // Class may already be registered from a previous instance
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_ERROR("Failed to register notification window class");
            return false;
        }
    }

    m_hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"GSRNotificationClass",
        L"GPU Screen Recorder Notification",
        WS_POPUP,
        0, 0, m_window_width, m_window_height,
        nullptr, nullptr, GetModuleHandle(nullptr), this
    );

    if (!m_hwnd) {
        LOG_ERROR("Failed to create notification window (error: %lu)", GetLastError());
        return false;
    }

    // Make it click-through and transparent
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    SetWindowLong(m_hwnd, GWL_EXSTYLE,
                  GetWindowLong(m_hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);

    return true;
}

// ─── DirectX 11 ─────────────────────────────────────────────────────────────

bool NotificationRenderer::create_d3d11() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL selected_level;

    DXGI_SWAP_CHAIN_DESC sc_desc = {};
    sc_desc.BufferCount = 2;
    sc_desc.BufferDesc.Width = m_window_width;
    sc_desc.BufferDesc.Height = m_window_height;
    sc_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.BufferDesc.RefreshRate.Numerator = 60;
    sc_desc.BufferDesc.RefreshRate.Denominator = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.OutputWindow = m_hwnd;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.Windowed = TRUE;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
        &sc_desc, &m_swap_chain, &m_d3d_device, &selected_level, &m_d3d_context
    );

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D11 for notifications (HRESULT: 0x%08X)",
                  static_cast<unsigned int>(hr));
        return false;
    }

    return true;
}

bool NotificationRenderer::create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = m_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr) || !back_buffer) return false;

    hr = m_d3d_device->CreateRenderTargetView(back_buffer, nullptr, &m_render_target);
    back_buffer->Release();
    return SUCCEEDED(hr);
}

void NotificationRenderer::cleanup_render_target() {
    if (m_render_target) { m_render_target->Release(); m_render_target = nullptr; }
}

// ─── Update Position ────────────────────────────────────────────────────────

void NotificationRenderer::update_window_position() {
    // Get cursor position
    POINT cursor;
    GetCursorPos(&cursor);

    // Find the monitor the cursor is on
    HMONITOR hMonitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor_info = { sizeof(MONITORINFO) };
    if (GetMonitorInfoA(hMonitor, &monitor_info)) {
        m_monitor_x = monitor_info.rcMonitor.left;
        m_monitor_y = monitor_info.rcMonitor.top;
        m_monitor_width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
        m_monitor_height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    }

    // Position notification in the top-right corner of the monitor
    // with some padding
    int padding = 20;
    int pos_x = m_monitor_x + m_monitor_width - m_window_width - padding;
    int pos_y = m_monitor_y + padding;

    SetWindowPos(m_hwnd, HWND_TOPMOST, pos_x, pos_y, m_window_width, m_window_height,
                 SWP_SHOWWINDOW);
}

// ─── Show Notification ──────────────────────────────────────────────────────

void NotificationRenderer::show_notification(const Notification& notification) {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    
    // Check priority: high priority interrupts current notification
    if (notification.priority > 0 && m_active) {
        // Interrupt current notification
        m_active.reset();
    }

    m_pending_notifications.push(notification);
}

bool NotificationRenderer::has_pending() const {
    std::lock_guard<std::mutex> lock(const_cast<NotificationRenderer*>(this)->m_queue_mutex);
    return !m_pending_notifications.empty() || m_active != nullptr;
}

// ─── Run Loop ───────────────────────────────────────────────────────────────

void NotificationRenderer::run() {
    m_running = true;

    MSG msg = {};
    while (m_running.load() && msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        auto now = std::chrono::steady_clock::now();

        // Check for next notification
        if (!m_active) {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_pending_notifications.empty()) {
                auto notif = m_pending_notifications.front();
                m_pending_notifications.pop();
                
                m_active = std::make_unique<ActiveNotification>();
                m_active->notification = notif;
                m_active->start_time = now;
                m_active->current_alpha = 0.0f;
                m_active->slide_offset = 50;

                m_showing = true;
                m_window_positioned = false;
            }
        }

        // Render active notification
        if (m_active) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_active->start_time).count();
            int duration = m_active->notification.duration_ms;

            // Fade in (first 200ms)
            if (elapsed < 200) {
                m_active->current_alpha = static_cast<float>(elapsed) / 200.0f;
                m_active->slide_offset = 50 - static_cast<int>(50.0f * elapsed / 200.0f);
            }
            // Hold
            else if (elapsed < duration - 500) {
                m_active->current_alpha = 1.0f;
                m_active->slide_offset = 0;
            }
            // Fade out (last 500ms)
            else if (elapsed < duration) {
                int fade_out = elapsed - (duration - 500);
                m_active->current_alpha = 1.0f - static_cast<float>(fade_out) / 500.0f;
            }
            // Done
            else {
                m_active.reset();
                m_showing = false;
                ShowWindow(m_hwnd, SW_HIDE);
                continue;
            }

            // Position window on first render
            if (!m_window_positioned) {
                update_window_position();
                m_window_positioned = true;
                ShowWindow(m_hwnd, SW_SHOW);
            }

            // Render
            render_notification(m_active->notification, m_active->current_alpha);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    m_running = false;
}

void NotificationRenderer::stop() {
    m_running = false;
    PostMessage(m_hwnd, WM_CLOSE, 0, 0);
}

// ─── Render Notification ────────────────────────────────────────────────────

void NotificationRenderer::render_notification(const Notification& notification, float alpha) {
    if (!m_d3d_context || !m_swap_chain || !m_render_target) return;

    // Clear with transparent color
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_d3d_context->OMSetRenderTargets(1, &m_render_target, nullptr);
    m_d3d_context->ClearRenderTargetView(m_render_target, clear_color);

    // Set up viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_window_width);
    viewport.Height = static_cast<float>(m_window_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_d3d_context->RSSetViewports(1, &viewport);

    // Draw background
    draw_background(m_window_width, m_window_height, alpha);

    // Draw icon
    draw_icon(notification.type, alpha);

    // Draw text
    draw_text(notification.title, notification.message, notification.subtitle,
              notification.type, m_window_width, m_window_height, alpha);

    // Present
    m_swap_chain->Present(1, 0);
}

// ─── Draw Background ────────────────────────────────────────────────────────

void NotificationRenderer::draw_background(int window_w, int window_h, float alpha) {
    // Simple colored rectangle using vertex shader
    // For a proper implementation, use a textured quad with rounded corners
    // Here we render a simple dark rectangle with the slide offset
    
    // Custom vertex shader approach would be ideal, but for simplicity
    // we use the D3D11 context to draw a colored quad
    
    // NOTE: Full implementation would use a vertex buffer + pixel shader
    // For now, this is a placeholder that the actual rendering happens
    // via a more complete implementation using D2D or pre-rendered textures
    
    // Since we can't easily draw primitives without a full shader setup,
    // we rely on the swap chain's alpha channel to create the transparent background.
    // The actual shadow/background effect would be implemented with D2D or custom shaders.
    
    // In a full implementation, draw a rounded rectangle here with:
    // - Dark background color (0.08, 0.08, 0.10, 0.85 * alpha)
    // - Green accent border on the left (0.1, 0.8, 0.3, alpha)
    // - Drop shadow effect
    
    // For now, the window is just a transparent overlay - the text
    // will be rendered using GDI text to a bitmap texture
}

// ─── Draw Icon ──────────────────────────────────────────────────────────────

void NotificationRenderer::draw_icon(NotificationType type, float alpha) {
    // In a full implementation, this would draw icons using D2D or preloaded textures:
    // - RecordingStarted: Red circle (●)
    // - RecordingStopped: Stop square (■)  
    // - ReplaySaved: Replay arrow (↺)
    // - ScreenshotTaken: Camera icon
    // - StreamingStarted: Broadcast icon
    // - Error: X mark
    // - Info: i mark
    
    // For now, the icon is represented by the text unicode characters
    // rendered in the draw_text function
}

// ─── Draw Text ──────────────────────────────────────────────────────────────

void NotificationRenderer::draw_text(const std::string& title, const std::string& message,
                                      const std::string& subtitle, NotificationType notif_type,
                                      int window_w, int window_h, float alpha) {
    // Use GDI to render text to a bitmap, then copy to D3D texture
    HDC hdc = GetDC(m_hwnd);
    if (!hdc) return;

    // Create a compatible DC for off-screen rendering
    HDC mem_dc = CreateCompatibleDC(hdc);
    if (!mem_dc) {
        ReleaseDC(m_hwnd, hdc);
        return;
    }

    // Create bitmap
    HBITMAP bitmap = CreateCompatibleBitmap(hdc, window_w, window_h);
    if (!bitmap) {
        DeleteDC(mem_dc);
        ReleaseDC(m_hwnd, hdc);
        return;
    }

    SelectObject(mem_dc, bitmap);

    // Set transparent background
    SetBkMode(mem_dc, TRANSPARENT);

    // Draw background rect for notification
    RECT bg_rect = { 0, 0, window_w, window_h };
    HBRUSH bg_brush = CreateSolidBrush(RGB(20, 20, 30));
    FillRect(mem_dc, &bg_rect, bg_brush);
    DeleteObject(bg_brush);

    // Draw left accent border (green)
    RECT accent_rect = { 0, 0, 4, window_h };
    HBRUSH accent_brush = CreateSolidBrush(RGB(25, 200, 75));
    FillRect(mem_dc, &accent_rect, accent_brush);
    DeleteObject(accent_brush);

    // Draw icon area (colored circle)
    int icon_size = 32;
    int icon_x = 16;
    int icon_y = (window_h - icon_size) / 2;

    HBRUSH icon_brush;
    COLORREF icon_color;
    switch (notif_type) {
        case NotificationType::RecordingStarted: icon_color = RGB(230, 50, 50); break;
        case NotificationType::RecordingStopped:
        case NotificationType::StreamingStopped: icon_color = RGB(230, 50, 50); break;
        case NotificationType::ReplaySaved:      icon_color = RGB(50, 150, 230); break;
        case NotificationType::ScreenshotTaken:  icon_color = RGB(230, 200, 50); break;
        case NotificationType::StreamingStarted: icon_color = RGB(230, 100, 0); break;
        case NotificationType::Error:            icon_color = RGB(230, 50, 50); break;
        default:                                 icon_color = RGB(100, 100, 100); break;
    }
    icon_brush = CreateSolidBrush(icon_color);

    // Draw filled circle
    HGDIOBJ old_brush = SelectObject(mem_dc, icon_brush);
    HPEN null_pen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
    HGDIOBJ old_pen = SelectObject(mem_dc, null_pen);
    Ellipse(mem_dc, icon_x, icon_y, icon_x + icon_size, icon_y + icon_size);
    SelectObject(mem_dc, old_pen);
    DeleteObject(null_pen);
    SelectObject(mem_dc, old_brush);
    DeleteObject(icon_brush);

    // Draw icon symbol
    const char* symbol = "";
    SetTextColor(mem_dc, RGB(255, 255, 255));
    switch (notif_type) {
        case NotificationType::RecordingStarted: symbol = "\xE2\x97\x8F"; break; // ●
        case NotificationType::RecordingStopped: symbol = "\xE2\x96\xA0"; break; // ■
        case NotificationType::ReplaySaved:      symbol = "\xE2\x86\xBA"; break; // ↺
        case NotificationType::ScreenshotTaken:  symbol = "\xE2\x8C\x98"; break; // ⌘
        case NotificationType::StreamingStarted: symbol = "\xE2\x96\xB6"; break; // ▶
        case NotificationType::StreamingStopped: symbol = "\xE2\x96\xA0"; break; // ■
        case NotificationType::Error:            symbol = "\xE2\x9C\x97"; break; // ✗
        default:                                 symbol = "\xE2\x84\xB9"; break; // ℹ
    }
    RECT symbol_rect = { icon_x, icon_y, icon_x + icon_size, icon_y + icon_size };
    HGDIOBJ old_font = SelectObject(mem_dc, m_font_title);
    DrawTextA(mem_dc, symbol, -1, &symbol_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem_dc, old_font);

    // Draw title text
    int text_x = icon_x + icon_size + 12;
    int text_y = 12;
    RECT title_rect = { text_x, text_y, window_w - 12, text_y + 24 };
    SelectObject(mem_dc, m_font_title);
    SetTextColor(mem_dc, RGB(240, 240, 240));
    DrawTextA(mem_dc, title.c_str(), -1, &title_rect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // Draw message text
    RECT msg_rect = { text_x, text_y + 24, window_w - 12, text_y + 44 };
    SelectObject(mem_dc, m_font_message);
    SetTextColor(mem_dc, RGB(180, 180, 180));
    DrawTextA(mem_dc, message.c_str(), -1, &msg_rect, DT_LEFT | DT_TOP | DT_SINGLELINE);

    // Draw subtitle text (file path, smaller, truncated)
    if (!subtitle.empty()) {
        RECT sub_rect = { text_x, text_y + 44, window_w - 12, text_y + 60 };
        SelectObject(mem_dc, m_font_subtitle);
        SetTextColor(mem_dc, RGB(130, 130, 130));
        DrawTextA(mem_dc, subtitle.c_str(), -1, &sub_rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }

    // NOT using alpha blending for simplicity here - in production code,
    // we'd use UpdateLayeredWindow with a BLENDFUNCTION for per-pixel alpha.
    // For now, we use the window's layered alpha.
    BITMAP bm;
    GetObject(bitmap, sizeof(bm), &bm);

    // Use window alpha for transparency effect
    BYTE window_alpha = static_cast<BYTE>(180 * alpha);
    SetLayeredWindowAttributes(m_hwnd, 0, window_alpha, LWA_ALPHA);

    // Redraw window
    PAINTSTRUCT ps;
    HDC paint_dc = BeginPaint(m_hwnd, &ps);
    if (paint_dc) {
        BitBlt(paint_dc, 0, 0, window_w, window_h, mem_dc, 0, 0, SRCCOPY);
        EndPaint(m_hwnd, &ps);
    }

    // Cleanup
    DeleteObject(bitmap);
    DeleteDC(mem_dc);
    ReleaseDC(m_hwnd, hdc);
}

// ─── Window Procedure ───────────────────────────────────────────────────────

LRESULT CALLBACK NotificationRenderer::window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    NotificationRenderer* renderer = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        renderer = static_cast<NotificationRenderer*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(renderer));
    } else {
        renderer = reinterpret_cast<NotificationRenderer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
        case WM_PAINT:
            // Handled in the render loop
            break;
        case WM_DESTROY:
            if (renderer) renderer->m_running = false;
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1; // Prevent flicker
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ─── Notification Manager ───────────────────────────────────────────────────

NotificationManager::NotificationManager() = default;

NotificationManager::~NotificationManager() {
    shutdown();
}

bool NotificationManager::initialize() {
    LOG_DEBUG("Notification manager initialized");
    return true;
}

void NotificationManager::shutdown() {
    if (m_renderer) {
        m_renderer->stop();
        m_renderer.reset();
    }
}

void NotificationManager::show(const Notification& notification) {
    if (m_renderer) {
        m_renderer->show_notification(notification);
    } else if (m_use_pipe) {
        send_remote_notification(notification);
    }
}

bool NotificationManager::connect_to_pipe(const std::string& pipe_name) {
    m_pipe_name = pipe_name;
    m_use_pipe = true;
    return true;
}

bool NotificationManager::send_remote_notification(const Notification& notification) {
    // This would connect to a named pipe and send the notification
    // For the standalone gsr-notification process
    LOG_DEBUG("Sending remote notification: %s", notification.title.c_str());
    return true;
}
