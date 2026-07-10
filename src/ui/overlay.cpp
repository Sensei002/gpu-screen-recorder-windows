#include "ui/overlay.h"
#include "ui/ui_renderer.h"
#include "common/log.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <windowsx.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ─── Constructor / Destructor ───────────────────────────────────────────────

Overlay::Overlay() = default;

Overlay::~Overlay() {
    shutdown();
}

// ─── Initialize ─────────────────────────────────────────────────────────────

bool Overlay::initialize(HINSTANCE instance, int nCmdShow) {
    if (!create_window(instance)) {
        LOG_ERROR("Failed to create overlay window");
        return false;
    }

    if (!create_d3d11()) {
        LOG_ERROR("Failed to create D3D11 for overlay");
        return false;
    }

    if (!create_render_target()) {
        LOG_ERROR("Failed to create render target for overlay");
        return false;
    }

    // Initialize UI renderer
    m_ui_renderer = std::make_unique<UIRenderer>();
    if (!m_ui_renderer->initialize(this)) {
        LOG_ERROR("Failed to initialize UI renderer");
        return false;
    }

    LOG_INFO("Overlay initialized successfully");
    return true;
}

void Overlay::shutdown() {
    stop();

    if (m_ui_renderer) {
        m_ui_renderer->shutdown();
        m_ui_renderer.reset();
    }

    cleanup_render_target();

    if (m_swap_chain) {
        m_swap_chain->Release();
        m_swap_chain = nullptr;
    }
    if (m_d3d_context) {
        m_d3d_context->Release();
        m_d3d_context = nullptr;
    }
    if (m_d3d_device) {
        m_d3d_device->Release();
        m_d3d_device = nullptr;
    }

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// ─── Window Creation ────────────────────────────────────────────────────────

bool Overlay::create_window(HINSTANCE instance) {
    m_wc.cbSize = sizeof(WNDCLASSEX);
    m_wc.style = CS_CLASSDC;
    m_wc.lpfnWndProc = window_proc;
    m_wc.hInstance = instance;
    m_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    m_wc.lpszClassName = L"GSROverlayClass";

    if (!RegisterClassEx(&m_wc)) {
        LOG_ERROR("Failed to register overlay window class");
        return false;
    }

    // Create a layered, transparent window
    m_hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"GSROverlayClass",
        L"GPU Screen Recorder",
        WS_POPUP,
        m_pos_x, m_pos_y, m_width, m_height,
        nullptr, nullptr, instance, this
    );

    if (!m_hwnd) {
        LOG_ERROR("Failed to create overlay window (error: %lu)", GetLastError());
        return false;
    }

    // Set layered window attributes: make semi-transparent
    SetLayeredWindowAttributes(m_hwnd, 0, 180, LWA_ALPHA);

    // Make the window click-through
    SetWindowLong(m_hwnd, GWL_EXSTYLE,
                  GetWindowLong(m_hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);

    LOG_DEBUG("Overlay window created: %dx%d", m_width, m_height);
    return true;
}

// ─── DirectX 11 ─────────────────────────────────────────────────────────────

bool Overlay::create_d3d11() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selected_level;

    // Swap chain description
    DXGI_SWAP_CHAIN_DESC sc_desc = {};
    sc_desc.BufferCount = 2;
    sc_desc.BufferDesc.Width = m_width;
    sc_desc.BufferDesc.Height = m_height;
    sc_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc_desc.BufferDesc.RefreshRate.Numerator = 60;
    sc_desc.BufferDesc.RefreshRate.Denominator = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.OutputWindow = m_hwnd;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.Windowed = TRUE;
    sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        feature_levels, ARRAYSIZE(feature_levels), D3D11_SDK_VERSION,
        &sc_desc, &m_swap_chain,
        &m_d3d_device, &selected_level, &m_d3d_context
    );

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create D3D11 device and swap chain (HRESULT: 0x%08X)",
                  static_cast<unsigned int>(hr));
        return false;
    }

    LOG_INFO("Overlay D3D11 initialized: feature level %d.%d",
             (selected_level >> 12) & 0xf, (selected_level >> 8) & 0xf);
    return true;
}

bool Overlay::create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    HRESULT hr = m_swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void**>(&back_buffer));
    if (FAILED(hr) || !back_buffer) {
        LOG_ERROR("Failed to get swap chain back buffer");
        return false;
    }

    hr = m_d3d_device->CreateRenderTargetView(back_buffer, nullptr, &m_render_target);
    back_buffer->Release();

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create render target view");
        return false;
    }

    return true;
}

void Overlay::cleanup_render_target() {
    if (m_render_target) {
        m_render_target->Release();
        m_render_target = nullptr;
    }
}

// ─── Show/Hide ──────────────────────────────────────────────────────────────

void Overlay::show() {
    if (!m_hwnd) return;
    m_visible = true;
    ShowWindow(m_hwnd, SW_SHOW);
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    LOG_DEBUG("Overlay shown");
}

void Overlay::hide() {
    if (!m_hwnd) return;
    m_visible = false;
    ShowWindow(m_hwnd, SW_HIDE);
    LOG_DEBUG("Overlay hidden");
}

void Overlay::toggle() {
    if (m_visible.load()) {
        hide();
    } else {
        show();
    }
}

// ─── Position / Size ───────────────────────────────────────────────────────

void Overlay::set_position(int x, int y) {
    m_pos_x = x;
    m_pos_y = y;
    if (m_hwnd) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, 0, 0,
                     SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

void Overlay::set_size(int width, int height) {
    m_width = width;
    m_height = height;
    if (m_hwnd) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, width, height,
                     SWP_NOMOVE | SWP_SHOWWINDOW);
    }
}

void Overlay::set_click_through(bool enabled) {
    m_click_through = enabled;
    if (m_hwnd) {
        LONG style = GetWindowLong(m_hwnd, GWL_EXSTYLE);
        if (enabled) {
            style |= WS_EX_TRANSPARENT;
        } else {
            style &= ~WS_EX_TRANSPARENT;
        }
        SetWindowLong(m_hwnd, GWL_EXSTYLE, style);
    }
}

// ─── Message Loop ───────────────────────────────────────────────────────────

void Overlay::run() {
    m_running = true;

    // Main message loop
    MSG msg = {};
    while (m_running.load() && msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // Render UI
        if (m_visible.load() && m_ui_renderer) {
            m_ui_renderer->render_frame();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    m_running = false;
}

void Overlay::stop() {
    m_running = false;
}

// ─── Window Procedure ───────────────────────────────────────────────────────

LRESULT CALLBACK Overlay::window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Overlay* overlay = nullptr;

    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        overlay = static_cast<Overlay*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    } else {
        overlay = reinterpret_cast<Overlay*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (!overlay) {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    // Pass events to ImGui
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (overlay->m_swap_chain && overlay->m_d3d_device) {
                overlay->cleanup_render_target();
                overlay->m_swap_chain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                                                      DXGI_FORMAT_UNKNOWN, 0);
                overlay->create_render_target();
            }
            return 0;

        case WM_DESTROY:
            overlay->m_running = false;
            PostQuitMessage(0);
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                overlay->hide();
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
            // Remove click-through when interacting
            overlay->set_click_through(false);
            break;

        case WM_LBUTTONUP:
            // Re-enable click-through if not hovering over controls
            // (let ImGui handle this)
            break;

        case WM_MOUSEMOVE:
            // Track mouse for UI interaction
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}
