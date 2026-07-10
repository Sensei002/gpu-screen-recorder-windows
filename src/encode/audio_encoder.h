#pragma once

#include "common/types.h"
#include <memory>
#include <atomic>
#include <cstdint>
#include <queue>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct SwrContext;

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    bool initialize(const AudioEncoderConfig& config);
    void shutdown();

    bool encode_frame(const AudioFrame& frame);
    void flush();

    struct EncodedPacket {
        std::vector<uint8_t> data;
        int64_t pts = 0;
        int64_t duration = 0;
    };

    bool get_encoded_packet(EncodedPacket& packet);

    // Codec info for muxer
    const AVCodecContext* codec_context() const { return m_codec_ctx; }
    int sample_rate() const { return m_config.sample_rate; }
    int channels() const { return m_config.channels; }
    int64_t frames_encoded() const { return m_frames_encoded; }
    bool is_initialized() const { return m_initialized; }

private:
    bool open_encoder();
    AVFrame* allocate_frame(int samples);

    // FFmpeg
    const AVCodec* m_codec = nullptr;
    AVCodecContext* m_codec_ctx = nullptr;
    AVFrame* m_frame = nullptr;

    // Audio resampling
    SwrContext* m_swr_ctx = nullptr;
    uint8_t** m_resampled_data = nullptr;
    int m_resampled_samples = 0;

    // Config
    AudioEncoderConfig m_config;
    bool m_initialized = false;

    // State
    std::atomic<int64_t> m_frames_encoded{0};
    int64_t m_pts = 0;

    // Packet queue
    std::queue<EncodedPacket> m_packet_queue;
};
