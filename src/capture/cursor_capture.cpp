#include "capture/cursor_capture.h"
#include "common/log.h"
#include <d3d11.h>
#include <dxgi1_2.h>

CursorCapture::CursorCapture() = default;

CursorCapture::~CursorCapture() {
    shutdown();
}

void CursorCapture::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;
    LOG_DEBUG("Cursor capture initialized");
}

void CursorCapture::shutdown() {
    if (m_cursor_bitmap) {
        delete[] m_cursor_bitmap;
        m_cursor_bitmap = nullptr;
    }
}

void CursorCapture::capture_cursor(IDXGIOutputDuplication* duplication) {
    if (!duplication) return;

    DXGI_OUTDUPL_FRAME_INFO frame_info;
    // We don't acquire here — this is called after AcquireNextFrame
    // The cursor info is stored in the frame info from the duplication

    // Get cursor position from Windows API
    POINT cursor_pos;
    if (GetCursorPos(&cursor_pos)) {
        m_cursor_x = cursor_pos.x;
        m_cursor_y = cursor_pos.y;
    }

    // Get cursor info
    CURSORINFO cursor_info = { sizeof(CURSORINFO) };
    if (GetCursorInfo(&cursor_info)) {
        m_cursor_visible = (cursor_info.flags & CURSOR_SHOWING) != 0;
        
        // Load cursor bitmap if we have a handle
        if (cursor_info.hCursor) {
            ICONINFO icon_info = {};
            if (GetIconInfo(cursor_info.hCursor, &icon_info)) {
                m_cursor_hotspot_x = static_cast<int>(icon_info.xHotspot);
                m_cursor_hotspot_y = static_cast<int>(icon_info.yHotspot);

                // Get the cursor bitmap
                if (icon_info.hbmColor) {
                    BITMAP bm;
                    if (GetObject(icon_info.hbmColor, sizeof(bm), &bm)) {
                        m_cursor_bitmap_width = bm.bmWidth;
                        m_cursor_bitmap_height = bm.bmHeight;
                        m_cursor_has_changed = true;
                    }
                }

                DeleteObject(icon_info.hbmMask);
                if (icon_info.hbmColor) {
                    DeleteObject(icon_info.hbmColor);
                }
            }
        }
    }
}

void CursorCapture::overlay_cursor(uint8_t* frame_data, int width, int height, int stride, PixelFormat format) {
    if (!frame_data || !m_cursor_visible) return;
    render_cursor_software(frame_data, width, height, stride);
}

void CursorCapture::render_cursor_software(uint8_t* frame_data, int width, int height, int stride) {
    const int cursor_size = 32; // Default cursor size
    const int half_size = cursor_size / 2;

    // Clamp cursor position to frame bounds
    int start_x = m_cursor_x - m_cursor_hotspot_x;
    int start_y = m_cursor_y - m_cursor_hotspot_y;

    // Software-rendered cursor (simple arrow/pointer shape)
    // In a full implementation, we'd use the actual cursor bitmap
    for (int y = 0; y < cursor_size; y++) {
        for (int x = 0; x < cursor_size; x++) {
            int px = start_x + x;
            int py = start_y + y;

            // Bounds check
            if (px < 0 || px >= width || py < 0 || py >= height) continue;

            // Simple arrow shape
            bool is_cursor_pixel = false;
            if (y <= x + 2 && x <= y + 4 && x < cursor_size - 2 && y < cursor_size - 2) {
                // Arrow body
                if (x < cursor_size / 2 && y < cursor_size - 4) {
                    is_cursor_pixel = true;
                }
                // Arrow tip
                if (x >= y && x >= cursor_size / 3) {
                    is_cursor_pixel = true;
                }
            }

            // Pointer shape
            int dx = x;
            int dy = y;
            if (dx < 16 && dy < 24) {
                // Arrow pointer shape
                bool left_edge = (dx == 0 && dy < 20);
                bool right_edge = (dy < 12 && dy > dx - 1) || (dy >= 12 && dy < 20 && dx > dy - 12);
                is_cursor_pixel = left_edge || right_edge || 
                                 (dy < 12 && dx <= dy) || 
                                 (dy >= 12 && dy < 20 && dx > 12 && dx < dy - 4);
            }

            if (is_cursor_pixel) {
                uint8_t* pixel = frame_data + py * stride + px * 4;
                // White cursor with black outline
                bool outline = (y == 0 || x == 0 || y == cursor_size - 1 || x == cursor_size - 1);
                
                // Check neighboring pixels for outline effect
                bool on_edge = false;
                int neighbors[][2] = {{-1,0},{1,0},{0,-1},{0,1},{-1,-1},{-1,1},{1,-1},{1,1}};
                for (auto& n : neighbors) {
                    int nx = x + n[0];
                    int ny = y + n[1];
                    if (nx >= 0 && nx < cursor_size && ny >= 0 && ny < cursor_size) {
                        if (!(ny <= nx + 2 && nx <= ny + 4)) {
                            on_edge = true;
                            break;
                        }
                    }
                }

                if (on_edge) {
                    pixel[0] = 0;      // B
                    pixel[1] = 0;      // G
                    pixel[2] = 0;      // R
                    pixel[3] = 255;    // A
                } else {
                    pixel[0] = 255;    // B
                    pixel[1] = 255;    // G
                    pixel[2] = 255;    // R
                    pixel[3] = 255;    // A
                }
            }
        }
    }
}
