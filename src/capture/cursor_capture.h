#pragma once

#include "common/types.h"
#include <memory>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct IDXGIOutputDuplication;

class CursorCapture {
public:
    CursorCapture();
    ~CursorCapture();

    void initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    // Capture cursor from frame info
    void capture_cursor(IDXGIOutputDuplication* duplication);

    // Overlay cursor onto a frame
    void overlay_cursor(uint8_t* frame_data, int width, int height, int stride, PixelFormat format);

    // Get cursor position (screen coordinates)
    void get_cursor_position(int& x, int& y) const { x = m_cursor_x; y = m_cursor_y; }

    // Check if cursor is currently visible
    bool is_cursor_visible() const { return m_cursor_visible; }

private:
    void render_cursor_shape(uint8_t* frame_data, int width, int height, int stride);
    void render_cursor_software(uint8_t* frame_data, int width, int height, int stride);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Cursor state
    bool m_cursor_visible = true;
    int m_cursor_x = 0;
    int m_cursor_y = 0;
    bool m_cursor_has_changed = false;

    // Cursor bitmap data (from DXGI cursor info)
    uint8_t* m_cursor_bitmap = nullptr;
    int m_cursor_bitmap_width = 0;
    int m_cursor_bitmap_height = 0;
    int m_cursor_hotspot_x = 0;
    int m_cursor_hotspot_y = 0;

    // Alpha blending
    bool m_supports_alpha = false;
};
