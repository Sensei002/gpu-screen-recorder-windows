#pragma once

#include "common/types.h"
#include <memory>
#include <atomic>
#include <vector>
#include <cstdint>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

class DirectNVENC;

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    // Initialize the encoder with given config
    bool initialize(const VideoEncoderConfig& config);
    void shutdown();

    // Encode a single frame
    // Returns true if encoded data is available (check via get_encoded_data)
    bool encode_frame(const VideoFrame& frame);
    bool encode_frame_nv12(const uint8_t* nv12_data, int width, int height, int64_t timestamp_us);

    // Flush remaining frames
    void flush();

    // Get encoded packet data (call after encode_frame returns true)
    struct EncodedPacket {
        std::vector<uint8_t> data;
        int64_t pts = 0;
        int64_t dts = 0;
        int64_t duration = 0;
        bool keyframe = false;
    };

    bool get_encoded_packet(EncodedPacket& packet);

    // Convert BGRA frame to NV12 (needed for most hardware encoders)
    bool convert_to_nv12(const VideoFrame& in_frame, uint8_t*& out_nv12, int& out_size);

    // Get the codec parameters (for muxer)
    // Returns the AVCodecContext — either from the FFmpeg encoder, or a
    // synthetic metadata context created for DirectNVENC encoding.
    const AVCodecContext* codec_context() const {
        return m_codec_ctx;
    }
    uint8_t* extra_data() const {
        return m_extradata;
    }
    int extra_data_size() const {
        return m_extradata_size;
    }

    // Get encoder info
    bool is_initialized() const { return m_initialized; }
    std::string encoder_name() const;
    int64_t frames_encoded() const { return m_frames_encoded; }

    // List available encoders
    static std::vector<std::string> list_available_encoders();
    static bool is_hardware_encoder_available(const std::string& name);

    // Set D3D11 device for hardware encoder interop
    void set_d3d11_device(void* d3d11_device) { m_d3d11_device = d3d11_device; }

private:
    bool open_encoder();
    bool try_open_with_direct_nvenc();
    AVFrame* allocate_frame(int width, int height, int format);
    void select_best_encoder();

    // FFmpeg
    const AVCodec* m_codec = nullptr;
    AVCodecContext* m_codec_ctx = nullptr;
    AVFrame* m_frame = nullptr;

    // Software conversion
    SwsContext* m_sws_ctx = nullptr;
    uint8_t* m_sws_buffer = nullptr;
    int m_sws_buffer_size = 0;

    // Direct NVENC (when FFmpeg's NVENC wrapper is incompatible)
    std::unique_ptr<DirectNVENC> m_direct_nvenc;

    // D3D11 device from ScreenCapture (for direct NVENC)
    void* m_d3d11_device = nullptr;  // ID3D11Device*

    // Config
    VideoEncoderConfig m_config;
    bool m_initialized = false;

    // State
    std::atomic<int64_t> m_frames_encoded{0};
    int64_t m_pts = 0;

    // Extradata for codec (needed by muxer) — FFmpeg path
    uint8_t* m_extradata = nullptr;
    int m_extradata_size = 0;

    // Encoded packet queue
    std::vector<EncodedPacket> m_packet_queue;
    size_t m_packet_read_index = 0;
};
