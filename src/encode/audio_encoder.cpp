#include "encode/audio_encoder.h"
#include "common/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <cstring>

AudioEncoder::AudioEncoder() = default;

AudioEncoder::~AudioEncoder() {
    shutdown();
}

bool AudioEncoder::initialize(const AudioEncoderConfig& config) {
    m_config = config;

    // Select codec
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    switch (config.codec) {
        case AudioCodec::AAC:  codec_id = AV_CODEC_ID_AAC;  break;
        case AudioCodec::Opus: codec_id = AV_CODEC_ID_OPUS; break;
        case AudioCodec::MP3:  codec_id = AV_CODEC_ID_MP3;  break;
        case AudioCodec::PCM:  codec_id = AV_CODEC_ID_PCM_S16LE; break;
    }

    m_codec = avcodec_find_encoder(codec_id);
    if (!m_codec) {
        LOG_ERROR("Failed to find audio encoder for codec %d", static_cast<int>(config.codec));
        return false;
    }

    return open_encoder();
}

bool AudioEncoder::open_encoder() {
    m_codec_ctx = avcodec_alloc_context3(m_codec);
    if (!m_codec_ctx) {
        LOG_ERROR("Failed to allocate audio codec context");
        return false;
    }

    // Configure
    m_codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP; // Planar float (most encoders prefer this)
    m_codec_ctx->sample_rate = m_config.sample_rate;
    m_codec_ctx->channels = m_config.channels;
    m_codec_ctx->channel_layout = av_get_default_channel_layout(m_config.channels);
    m_codec_ctx->bit_rate = m_config.bitrate_kbps * 1000;

    // Set timebase
    m_codec_ctx->time_base = AVRational{1, m_config.sample_rate};

    // AAC specific options
    if (m_config.codec == AudioCodec::AAC) {
        m_codec_ctx->profile = FF_PROFILE_AAC_LOW;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 0, 0)
        // Use the new API for setting AAC options
        av_opt_set_int(m_codec_ctx->priv_data, "aac_coder", 2, 0); // Twisted
#else
        av_opt_set(m_codec_ctx->priv_data, "aac_coder", "twoloop", 0);
#endif
    }

    // Opus specific options
    if (m_config.codec == AudioCodec::Opus) {
        av_opt_set_int(m_codec_ctx->priv_data, "application", 2048, 0); // AV_OPT_TYPE_INT, AV_OPUS_APP_AUDIO
    }

    int ret = avcodec_open2(m_codec_ctx, m_codec, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to open audio encoder: %s", errbuf);
        return false;
    }

    m_initialized = true;
    LOG_INFO("Audio encoder initialized: %s (%d Hz, %d ch, %d kbps)",
             avcodec_get_name(m_codec->id), m_config.sample_rate,
             m_config.channels, m_config.bitrate_kbps);
    return true;
}

void AudioEncoder::shutdown() {
    if (m_initialized) {
        flush();
    }

    if (m_resampled_data) {
        av_freep(&m_resampled_data[0]);
        av_freep(&m_resampled_data);
    }

    if (m_swr_ctx) {
        swr_free(&m_swr_ctx);
    }

    if (m_frame) {
        av_frame_free(&m_frame);
    }

    if (m_codec_ctx) {
        avcodec_free_context(&m_codec_ctx);
    }

    m_initialized = false;
}

bool AudioEncoder::encode_frame(const AudioFrame& frame) {
    if (!m_initialized || !m_codec_ctx) return false;

    // Create input frame from raw PCM data
    AVFrame* input_frame = av_frame_alloc();
    if (!input_frame) return false;

    input_frame->nb_samples = frame.samples;
    input_frame->sample_rate = frame.sample_rate;
    input_frame->channels = frame.channels;
    input_frame->channel_layout = av_get_default_channel_layout(frame.channels);
    input_frame->format = AV_SAMPLE_FMT_S16; // WASAPI typically gives us S16LE

    int ret = av_frame_get_buffer(input_frame, 0);
    if (ret < 0) {
        av_frame_free(&input_frame);
        return false;
    }

    // Copy data (WASAPI gives interleaved S16LE)
    size_t data_size = static_cast<size_t>(frame.samples) * frame.channels * 2; // 2 bytes per sample (S16)
    if (data_size > 0 && frame.data) {
        memcpy(input_frame->data[0], frame.data, 
               std::min(data_size, static_cast<size_t>(frame.samples) * frame.channels * 2));
    }

    // Resample if needed (convert S16 to FLTP, match sample rate)
    if (!m_swr_ctx) {
        m_swr_ctx = swr_alloc_set_opts(nullptr,
            m_codec_ctx->channel_layout, m_codec_ctx->sample_fmt, m_codec_ctx->sample_rate,
            input_frame->channel_layout, static_cast<AVSampleFormat>(input_frame->format), 
            input_frame->sample_rate,
            0, nullptr);

        if (!m_swr_ctx || swr_init(m_swr_ctx) < 0) {
            LOG_ERROR("Failed to initialize audio resampler");
            av_frame_free(&input_frame);
            return false;
        }
    }

    // Calculate output samples
    int out_samples = static_cast<int>(av_rescale_rnd(
        swr_get_delay(m_swr_ctx, input_frame->sample_rate) + input_frame->nb_samples,
        m_codec_ctx->sample_rate, input_frame->sample_rate, AV_ROUND_UP));

    // Allocate resampled buffer
    if (m_resampled_samples < out_samples) {
        if (m_resampled_data) {
            av_freep(&m_resampled_data[0]);
        }
        av_samples_alloc(&m_resampled_data, nullptr, m_codec_ctx->channels,
                         out_samples, m_codec_ctx->sample_fmt, 0);
        m_resampled_samples = out_samples;
    }

    // Perform resampling
    int ret_swr = swr_convert(m_swr_ctx, m_resampled_data, out_samples,
                              const_cast<const uint8_t**>(input_frame->data), input_frame->nb_samples);
    av_frame_free(&input_frame);

    if (ret_swr < 0) {
        LOG_ERROR("Audio resampling failed");
        return false;
    }

    if (ret_swr == 0) return true; // No data to encode

    // Send to encoder
    if (!m_frame) {
        m_frame = allocate_frame(m_codec_ctx->frame_size);
        if (!m_frame) return false;
    }

    // We need to send audio in frame_size chunks
    int samples_sent = 0;
    while (samples_sent < ret_swr) {
        int frame_samples = std::min(m_codec_ctx->frame_size, ret_swr - samples_sent);
        
        m_frame->nb_samples = frame_samples;
        m_frame->pts = m_pts;
        m_pts += frame_samples;

        // Copy resampled data
        for (int ch = 0; ch < m_codec_ctx->channels; ch++) {
            size_t copy_size = static_cast<size_t>(frame_samples) * 
                               av_get_bytes_per_sample(m_codec_ctx->sample_fmt);
            memcpy(m_frame->data[ch], m_resampled_data[ch] + samples_sent * 
                   av_get_bytes_per_sample(m_codec_ctx->sample_fmt), copy_size);
        }
        samples_sent += frame_samples;

        // Encode
        ret = avcodec_send_frame(m_codec_ctx, m_frame);
        if (ret < 0) {
            char errbuf[256];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error sending audio frame: %s", errbuf);
            continue;
        }

        // Receive packets
        while (ret >= 0) {
            AVPacket* packet = av_packet_alloc();
            ret = avcodec_receive_packet(m_codec_ctx, packet);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&packet);
                break;
            }

            if (ret < 0) {
                av_packet_free(&packet);
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Error receiving audio packet: %s", errbuf);
                break;
            }

            EncodedPacket encoded;
            encoded.data.resize(packet->size);
            memcpy(encoded.data.data(), packet->data, packet->size);
            encoded.pts = packet->pts;
            encoded.duration = packet->duration;

            m_packet_queue.push(encoded);
            m_frames_encoded++;

            av_packet_free(&packet);
        }
    }

    return true;
}

bool AudioEncoder::get_encoded_packet(EncodedPacket& packet) {
    if (m_packet_queue.empty()) return false;
    packet = m_packet_queue.front();
    m_packet_queue.pop();
    return true;
}

void AudioEncoder::flush() {
    if (!m_initialized || !m_codec_ctx) return;

    int ret = avcodec_send_frame(m_codec_ctx, nullptr);
    if (ret < 0) return;

    while (ret >= 0) {
        AVPacket* packet = av_packet_alloc();
        ret = avcodec_receive_packet(m_codec_ctx, packet);
        
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&packet);
            break;
        }

        if (ret >= 0) {
            EncodedPacket encoded;
            encoded.data.resize(packet->size);
            memcpy(encoded.data.data(), packet->data, packet->size);
            encoded.pts = packet->pts;
            encoded.duration = packet->duration;

            m_packet_queue.push(encoded);
            av_packet_free(&packet);
        }
    }
}

AVFrame* AudioEncoder::allocate_frame(int samples) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->nb_samples = samples;
    frame->format = m_codec_ctx->sample_fmt;
    frame->channels = m_codec_ctx->channels;
    frame->channel_layout = m_codec_ctx->channel_layout;
    frame->sample_rate = m_codec_ctx->sample_rate;

    int ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    return frame;
}
