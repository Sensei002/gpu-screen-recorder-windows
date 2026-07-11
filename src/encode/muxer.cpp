#include "encode/muxer.h"
#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/timestamp.h>
}

#include <algorithm>

Muxer::Muxer() = default;

Muxer::~Muxer() {
    close();
}

bool Muxer::initialize_file(const std::string& file_path,
                            const AVCodecContext* video_ctx,
                            const AVCodecContext* audio_ctx,
                            const uint8_t* video_extradata,
                            int video_extradata_size) {
    m_output_path = file_path;

    if (!init_format(file_path, video_ctx, audio_ctx)) {
        return false;
    }

    // Copy extradata to video stream
    if (video_extradata && video_extradata_size > 0 && m_video_stream) {
        av_free(m_video_stream->codecpar->extradata);
        m_video_stream->codecpar->extradata = static_cast<uint8_t*>(
            av_mallocz(video_extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (m_video_stream->codecpar->extradata) {
            memcpy(m_video_stream->codecpar->extradata, video_extradata, video_extradata_size);
            m_video_stream->codecpar->extradata_size = video_extradata_size;
        }
    }

    // Write header
    int ret = avformat_write_header(m_fmt_ctx, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to write header to %s: %s", file_path.c_str(), errbuf);
        close();
        return false;
    }

    m_header_written = true;
    m_initialized = true;

    LOG_INFO("Muxer initialized: %s", file_path.c_str());
    return true;
}

bool Muxer::initialize_stream(const std::string& url,
                              const AVCodecContext* video_ctx,
                              const AVCodecContext* audio_ctx) {
    m_output_path = url;
    return init_format(url, video_ctx, audio_ctx);
}

bool Muxer::init_format(const std::string& output,
                        const AVCodecContext* video_ctx,
                        const AVCodecContext* audio_ctx) {
    // Determine format from file extension or URL
    int ret = avformat_alloc_output_context2(&m_fmt_ctx, nullptr, nullptr, output.c_str());
    if (ret < 0 || !m_fmt_ctx) {
        // Try MP4 as default
        ret = avformat_alloc_output_context2(&m_fmt_ctx, nullptr, "mp4", output.c_str());
        if (ret < 0 || !m_fmt_ctx) {
            LOG_ERROR("Failed to create output context for %s", output.c_str());
            return false;
        }
    }

    LOG_INFO("Output format: %s (%s)", m_fmt_ctx->oformat->name, output.c_str());

    // Create video stream
    if (video_ctx) {
        m_video_stream = avformat_new_stream(m_fmt_ctx, nullptr);
        if (!m_video_stream) {
            LOG_ERROR("Failed to create video stream");
            return false;
        }

        m_video_stream->id = 0;
        m_video_stream->time_base = video_ctx->time_base;

        // Copy codec parameters
        ret = avcodec_parameters_from_context(m_video_stream->codecpar, video_ctx);
        if (ret < 0) {
            LOG_ERROR("Failed to copy video codec parameters");
            return false;
        }

        LOG_INFO("Video stream: %dx%d, codec: %s",
                 video_ctx->width, video_ctx->height,
                 avcodec_get_name(video_ctx->codec_id));
    }

    // Create audio stream
    if (audio_ctx) {
        m_audio_stream = avformat_new_stream(m_fmt_ctx, nullptr);
        if (!m_audio_stream) {
            LOG_ERROR("Failed to create audio stream");
            return false;
        }

        m_audio_stream->id = 1;
        m_audio_stream->time_base = audio_ctx->time_base;

        ret = avcodec_parameters_from_context(m_audio_stream->codecpar, audio_ctx);
        if (ret < 0) {
            LOG_ERROR("Failed to copy audio codec parameters");
            return false;
        }

        int audio_channels = 0;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
        audio_channels = audio_ctx->ch_layout.nb_channels;
#else
        audio_channels = audio_ctx->channels;
#endif
        LOG_INFO("Audio stream: %d Hz, %d ch, codec: %s",
                 audio_ctx->sample_rate,
                 audio_channels,
                 avcodec_get_name(audio_ctx->codec_id));
    }

    // Open file for writing
    if (!(m_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_fmt_ctx->pb, output.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to open output file %s: %s", output.c_str(), errbuf);
            return false;
        }
    }

    // Save timebases
    if (video_ctx) m_video_time_base = video_ctx->time_base;
    if (audio_ctx) m_audio_time_base = audio_ctx->time_base;

    return true;
}

bool Muxer::write_video_packet(const uint8_t* data, int size,
                               int64_t pts, int64_t dts, int64_t duration,
                               bool keyframe) {
    if (!m_initialized || !m_header_written || !m_video_stream) return false;

    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    packet->data = const_cast<uint8_t*>(data);
    packet->size = size;

    // Rescale timestamps
    AVRational packet_tb{1, AV_TIME_BASE};
    packet->pts = av_rescale_q(pts, m_video_time_base, m_video_stream->time_base);
    packet->dts = av_rescale_q(dts, m_video_time_base, m_video_stream->time_base);
    packet->duration = av_rescale_q(duration, m_video_time_base, m_video_stream->time_base);
    packet->stream_index = m_video_stream->index;

    if (keyframe) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    int ret = av_interleaved_write_frame(m_fmt_ctx, packet);
    av_packet_free(&packet);

    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to write video packet: %s", errbuf);
        return false;
    }

    return true;
}

bool Muxer::write_audio_packet(const uint8_t* data, int size,
                               int64_t pts, int64_t duration) {
    if (!m_initialized || !m_header_written || !m_audio_stream) return false;

    AVPacket* packet = av_packet_alloc();
    if (!packet) return false;

    packet->data = const_cast<uint8_t*>(data);
    packet->size = size;
    packet->pts = av_rescale_q(pts, m_audio_time_base, m_audio_stream->time_base);
    packet->dts = packet->pts; // Audio usually has no B-frames
    packet->duration = av_rescale_q(duration, m_audio_time_base, m_audio_stream->time_base);
    packet->stream_index = m_audio_stream->index;

    int ret = av_interleaved_write_frame(m_fmt_ctx, packet);
    av_packet_free(&packet);

    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to write audio packet: %s", errbuf);
        return false;
    }

    return true;
}

bool Muxer::finalize() {
    if (!m_initialized) return false;

    if (m_header_written && m_fmt_ctx) {
        int ret = av_write_trailer(m_fmt_ctx);
        if (ret < 0) {
            LOG_WARN("Error writing trailer: %d", ret);
        }
    }

    close();
    LOG_INFO("Muxer finalized: %s", m_output_path.c_str());
    return true;
}

int64_t Muxer::file_size() const {
    if (m_fmt_ctx && m_fmt_ctx->pb) {
        return avio_size(m_fmt_ctx->pb);
    }
    return 0;
}

double Muxer::duration_seconds() const {
    if (m_video_stream && m_video_stream->time_base.den > 0) {
        return static_cast<double>(m_video_stream->duration) * 
               m_video_stream->time_base.num / m_video_stream->time_base.den;
    }
    return 0.0;
}

void Muxer::close() {
    if (m_fmt_ctx && !(m_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (m_fmt_ctx->pb) {
            avio_closep(&m_fmt_ctx->pb);
        }
    }
    if (m_fmt_ctx) {
        avformat_free_context(m_fmt_ctx);
        m_fmt_ctx = nullptr;
    }
    m_video_stream = nullptr;
    m_audio_stream = nullptr;
    m_initialized = false;
    m_header_written = false;
}
