#pragma once

#include <cstdint>
#include <vector>
#include <string>

struct ID3D11Device;

// ─── DirectNVENC ───────────────────────────────────────────────────────────
// A lightweight NVENC wrapper that loads nvEncodeAPI.dll at runtime and calls
// the NVENC SDK C API directly, bypassing FFmpeg's h264_nvenc wrapper.
// This is necessary because recent FFmpeg builds use NVENC SDK headers that
// are too new for Maxwell-era GPUs, causing avcodec_open2() to return ENOSYS.
//
// Follows the same approach as OBS Studio's obs-nvenc plugin.

class DirectNVENC {
public:
    DirectNVENC();
    ~DirectNVENC();

    DirectNVENC(const DirectNVENC&) = delete;
    DirectNVENC& operator=(const DirectNVENC&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    bool initialize(ID3D11Device* d3d11_device,
                    int width, int height, int fps,
                    int quality, bool constant_quality, int bitrate_kbps);
    void shutdown();
    bool is_initialized() const { return m_session != nullptr; }

    // ── Encoding ───────────────────────────────────────────────────────────

    bool encode_frame(const uint8_t* nv12_data, int width, int height, int64_t timestamp_us);
    void flush();

    // ── Output ─────────────────────────────────────────────────────────────

    struct EncodedPacket {
        std::vector<uint8_t> data;
        int64_t pts = 0;
        int64_t dts = 0;
        int64_t duration = 0;
        bool keyframe = false;
    };

    bool get_encoded_packet(EncodedPacket& packet);

    // ── Codec info (for muxer) ─────────────────────────────────────────────

    const uint8_t* extra_data() const { return m_extradata.data(); }
    int extra_data_size() const { return static_cast<int>(m_extradata.size()); }
    std::string encoder_name() const { return "h264_nvenc_direct"; }
    int64_t frames_encoded() const { return m_frames_encoded; }

    // ── Static helpers ─────────────────────────────────────────────────────

    static bool is_available();

private:
    // Internal types use the official NV_ENC types from ffnvcodec/nvEncodeAPI.h
    // (included only in .cpp)

    bool load_library();
    void unload_library();
    bool create_session();
    bool configure_encoder();
    bool create_buffers();
    bool get_sequence_params();

    // State
    void*       m_module       = nullptr; // HMODULE
    void*       m_funcs        = nullptr; // NV_ENCODE_API_FUNCTION_LIST*
    void*       m_session      = nullptr;
    void*       m_input_buffer = nullptr;
    void*       m_output_buffer = nullptr;
    ID3D11Device* m_device     = nullptr;

    int      m_width           = 0;
    int      m_height          = 0;
    int      m_fps             = 0;
    int      m_quality         = 23;
    bool     m_constant_quality = true;
    int      m_bitrate_kbps    = 10000;

    std::vector<uint8_t> m_extradata;
    std::vector<EncodedPacket> m_packet_queue;
    size_t   m_packet_read_index = 0;

    int64_t  m_frames_encoded = 0;
    int64_t  m_pts_counter    = 0;
    bool     m_first_frame    = true;
};
