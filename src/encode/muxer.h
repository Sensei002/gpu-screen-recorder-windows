#pragma once

#include "common/types.h"
#include <string>
#include <memory>
#include <atomic>
#include <cstdint>

struct AVFormatContext;
struct AVStream;
struct AVCodecContext;

class Muxer {
public:
    Muxer();
    ~Muxer();

    // Initialize muxer for file output
    bool initialize_file(const std::string& file_path,
                         const AVCodecContext* video_ctx,
                         const AVCodecContext* audio_ctx,
                         const uint8_t* video_extradata,
                         int video_extradata_size);

    // Initialize muxer for streaming
    bool initialize_stream(const std::string& url,
                           const AVCodecContext* video_ctx,
                           const AVCodecContext* audio_ctx);

    // Write a video packet
    bool write_video_packet(const uint8_t* data, int size,
                            int64_t pts, int64_t dts, int64_t duration,
                            bool keyframe);

    // Write an audio packet
    bool write_audio_packet(const uint8_t* data, int size,
                            int64_t pts, int64_t duration);

    // Finalize the file
    bool finalize();

    // Get info
    bool is_initialized() const { return m_initialized; }
    const std::string& output_path() const { return m_output_path; }
    int64_t file_size() const;
    double duration_seconds() const;

    // Get the AVFormatContext for direct access
    AVFormatContext* format_context() const { return m_fmt_ctx; }

private:
    bool init_format(const std::string& output,
                     const AVCodecContext* video_ctx,
                     const AVCodecContext* audio_ctx);
    void close();

    // FFmpeg
    AVFormatContext* m_fmt_ctx = nullptr;
    AVStream* m_video_stream = nullptr;
    AVStream* m_audio_stream = nullptr;

    // Context for muxing
    AVCodecContext* m_video_codec_ctx_copy = nullptr;
    AVCodecContext* m_audio_codec_ctx_copy = nullptr;

    // State
    bool m_initialized = false;
    bool m_header_written = false;
    std::string m_output_path;

    // Timing
    int64_t m_video_start_pts = -1;
    int64_t m_audio_start_pts = -1;
    AVRational m_video_time_base{1, 90000};
    AVRational m_audio_time_base{1, 48000};
};
