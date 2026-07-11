#include "encode/video_encoder.h"
#include "common/log.h"

// Include d3d11.h BEFORE extern "C" FFmpeg headers to prevent conflicts
// with Windows SDK operator== and operator!= overloads inside extern "C" linkage.
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <algorithm>

// ─── Constructor / Destructor ───────────────────────────────────────────────

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder() {
    shutdown();
}

// ─── List Available Encoders ────────────────────────────────────────────────

std::vector<std::string> VideoEncoder::list_available_encoders() {
    std::vector<std::string> encoders;

    const AVCodec* codec = nullptr;
    void* opaque = nullptr;
    while ((codec = av_codec_iterate(&opaque))) {
        if (av_codec_is_encoder(codec)) {
            encoders.push_back(codec->name);
        }
    }

    return encoders;
}

bool VideoEncoder::is_hardware_encoder_available(const std::string& name) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    return codec != nullptr;
}

// ─── Initialize ─────────────────────────────────────────────────────────────

bool VideoEncoder::initialize(const VideoEncoderConfig& config) {
    m_config = config;
    select_best_encoder();
    return open_encoder();
}

void VideoEncoder::select_best_encoder() {
    // If user specified an encoder, try it
    if (!m_config.encoder_name.empty()) {
        m_codec = avcodec_find_encoder_by_name(m_config.encoder_name.c_str());
        if (m_codec) {
            LOG_INFO("Using user-specified encoder: %s", m_config.encoder_name.c_str());
            return;
        }
        LOG_WARN("User-specified encoder '%s' not found, auto-selecting", 
                 m_config.encoder_name.c_str());
    }

    // Auto-select best encoder based on codec and available hardware
    const char* codec_name = nullptr;
    const char* hw_encoder = nullptr;
    const char* sw_encoder = nullptr;

    switch (m_config.codec) {
        case VideoCodec::H264:
            #ifdef GSR_ENABLE_NVENC
            hw_encoder = "h264_nvenc";
            #endif
            #ifdef GSR_ENABLE_AMF
            if (!hw_encoder) hw_encoder = "h264_amf";
            #endif
            #ifdef GSR_ENABLE_QSV
            if (!hw_encoder) hw_encoder = "h264_qsv";
            #endif
            sw_encoder = "libx264";
            break;

        case VideoCodec::HEVC:
            #ifdef GSR_ENABLE_NVENC
            hw_encoder = "hevc_nvenc";
            #endif
            #ifdef GSR_ENABLE_AMF
            if (!hw_encoder) hw_encoder = "hevc_amf";
            #endif
            #ifdef GSR_ENABLE_QSV
            if (!hw_encoder) hw_encoder = "hevc_qsv";
            #endif
            sw_encoder = "libx265";
            break;

        case VideoCodec::AV1:
            #ifdef GSR_ENABLE_NVENC
            hw_encoder = "av1_nvenc";
            #endif
            sw_encoder = "libaom-av1";
            break;

        case VideoCodec::VP8:
            sw_encoder = "libvpx";
            break;

        case VideoCodec::VP9:
            sw_encoder = "libvpx-vp9";
            break;
    }

    // Try hardware encoder first
    if (hw_encoder) {
        m_codec = avcodec_find_encoder_by_name(hw_encoder);
        if (m_codec) {
            LOG_INFO("Selected hardware encoder: %s", hw_encoder);
            return;
        }
    }

    // Fall back to software encoder
    if (sw_encoder) {
        m_codec = avcodec_find_encoder_by_name(sw_encoder);
        if (m_codec) {
            LOG_INFO("Selected software encoder: %s", sw_encoder);
            return;
        }
    }

    // Final fallback: find any encoder for this codec
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    switch (m_config.codec) {
        case VideoCodec::H264: codec_id = AV_CODEC_ID_H264; break;
        case VideoCodec::HEVC: codec_id = AV_CODEC_ID_HEVC; break;
        case VideoCodec::AV1:  codec_id = AV_CODEC_ID_AV1;  break;
        case VideoCodec::VP8:  codec_id = AV_CODEC_ID_VP8;  break;
        case VideoCodec::VP9:  codec_id = AV_CODEC_ID_VP9;  break;
    }

    m_codec = avcodec_find_encoder(codec_id);
    if (m_codec) {
        LOG_INFO("Selected fallback encoder: %s", m_codec->name);
    }
}

bool VideoEncoder::open_encoder() {
    if (!m_codec) {
        LOG_ERROR("No encoder found for requested codec");
        return false;
    }

    // Allocate codec context
    m_codec_ctx = avcodec_alloc_context3(m_codec);
    if (!m_codec_ctx) {
        LOG_ERROR("Failed to allocate codec context");
        return false;
    }

    // Configure encoder
    m_codec_ctx->width = m_config.width;
    m_codec_ctx->height = m_config.height;
    m_codec_ctx->time_base = AVRational{1, m_config.fps};
    m_codec_ctx->framerate = AVRational{m_config.fps, 1};
    m_codec_ctx->pix_fmt = AV_PIX_FMT_NV12; // Most hardware encoders use NV12
    m_codec_ctx->gop_size = m_config.fps * m_config.keyint_seconds;
    m_codec_ctx->max_b_frames = 0; // Low latency

    // Quality / bitrate settings
    if (m_config.constant_quality) {
        // CRF / CQ mode
        m_codec_ctx->flags |= AV_CODEC_FLAG_QSCALE;
        m_codec_ctx->global_quality = m_config.quality;

        // Set codec-specific quality options
        const char* quality_param = nullptr;
        if (strstr(m_codec->name, "nvenc")) {
            quality_param = "cq";
        } else if (strstr(m_codec->name, "amf")) {
            quality_param = "quality";
        } else if (strstr(m_codec->name, "qsv")) {
            quality_param = "global_quality";
        } else if (strstr(m_codec->name, "libx264") || strstr(m_codec->name, "libx265")) {
            quality_param = "crf";
        }

        if (quality_param) {
            av_opt_set_int(m_codec_ctx->priv_data, quality_param, m_config.quality, 0);
        }

        LOG_INFO("Using constant quality mode: %s=%d", 
                 quality_param ? quality_param : "global_quality", m_config.quality);
    } else {
        // CBR/VBR mode
        m_codec_ctx->bit_rate = m_config.bitrate_kbps * 1000;
        m_codec_ctx->rc_min_rate = m_config.bitrate_kbps * 1000;
        m_codec_ctx->rc_max_rate = m_config.bitrate_kbps * 1000;
        m_codec_ctx->rc_buffer_size = m_config.bitrate_kbps * 1000 / 2;

        LOG_INFO("Using bitrate mode: %d kbps", m_config.bitrate_kbps);
    }

    // Set codec-specific options for hardware encoders
    if (strstr(m_codec->name, "nvenc")) {
        // NVENC specific options
        av_opt_set(m_codec_ctx->priv_data, "preset", "p6", 0);
        av_opt_set(m_codec_ctx->priv_data, "tune", "hq", 0);
        av_opt_set(m_codec_ctx->priv_data, "rc", m_config.constant_quality ? "vbr" : "cbr", 0);
        av_opt_set_int(m_codec_ctx->priv_data, "delay", 0, 0); // Low latency
        av_opt_set_int(m_codec_ctx->priv_data, "b_ref_mode", 0, 0); // No B-frames

        // HDR support
        if (m_config.enable_hdr) {
            av_opt_set(m_codec_ctx->priv_data, "color_range", "2", 0); // Full range
            av_opt_set(m_codec_ctx->priv_data, "colorspace", "9", 0);  // BT.2020
            av_opt_set(m_codec_ctx->priv_data, "color_trc", "16", 0);  // SMPTE ST 2084
            av_opt_set(m_codec_ctx->priv_data, "color_primaries", "9", 0); // BT.2020
        }
    } else if (strstr(m_codec->name, "amf")) {
        av_opt_set(m_codec_ctx->priv_data, "usage", "ultralowlatency", 0);
        av_opt_set(m_codec_ctx->priv_data, "quality", "quality", 0);
        av_opt_set_int(m_codec_ctx->priv_data, "bframe_num", 0, 0);
        av_opt_set_int(m_codec_ctx->priv_data, "bframe_ref", 0, 0);
    } else if (strstr(m_codec->name, "qsv")) {
        av_opt_set(m_codec_ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set_int(m_codec_ctx->priv_data, "async_depth", 1, 0);
    } else if (strstr(m_codec->name, "libx264") || strstr(m_codec->name, "libx265")) {
        av_opt_set(m_codec_ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(m_codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    // Open the codec
    int ret = avcodec_open2(m_codec_ctx, m_codec, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to open encoder: %s", errbuf);
        return false;
    }

    // Save extradata for muxer
    if (m_codec_ctx->extradata && m_codec_ctx->extradata_size > 0) {
        m_extradata = static_cast<uint8_t*>(av_malloc(m_codec_ctx->extradata_size));
        if (m_extradata) {
            memcpy(m_extradata, m_codec_ctx->extradata, m_codec_ctx->extradata_size);
            m_extradata_size = m_codec_ctx->extradata_size;
        }
    }

    m_initialized = true;
    LOG_INFO("Encoder initialized: %s (%s)", m_codec->name,
             m_config.constant_quality ? "CQ mode" : "bitrate mode");
    return true;
}

void VideoEncoder::shutdown() {
    if (m_initialized) {
        flush();
    }

    // Free the frame
    if (m_frame) {
        av_frame_free(&m_frame);
    }

    // Free sws context and buffers
    if (m_sws_ctx) {
        sws_freeContext(m_sws_ctx);
        m_sws_ctx = nullptr;
    }
    if (m_sws_buffer) {
        av_free(m_sws_buffer);
        m_sws_buffer = nullptr;
    }

    // Free extradata
    if (m_extradata) {
        av_free(m_extradata);
        m_extradata = nullptr;
    }

    if (m_codec_ctx) {
        avcodec_free_context(&m_codec_ctx);
    }

    m_initialized = false;
    LOG_DEBUG("Encoder shutdown");
}

// ─── Encoding ───────────────────────────────────────────────────────────────

bool VideoEncoder::encode_frame(const VideoFrame& frame) {
    if (!m_initialized) return false;

    // Convert BGRA to NV12
    uint8_t* nv12_data = nullptr;
    int nv12_size = 0;

    if (!convert_to_nv12(frame, nv12_data, nv12_size)) {
        return false;
    }

    bool result = encode_frame_nv12(nv12_data, frame.width, frame.height, frame.timestamp_us);
    av_free(nv12_data);
    return result;
}

bool VideoEncoder::encode_frame_nv12(const uint8_t* nv12_data, int width, int height, int64_t timestamp_us) {
    if (!m_initialized || !m_codec_ctx) return false;

    // Allocate or reuse AVFrame
    if (!m_frame) {
        m_frame = allocate_frame(m_codec_ctx->width, m_codec_ctx->height, AV_PIX_FMT_NV12);
        if (!m_frame) return false;
    }

    // Fill the frame with NV12 data
    av_image_fill_arrays(m_frame->data, m_frame->linesize,
                         const_cast<uint8_t*>(nv12_data),
                         AV_PIX_FMT_NV12, width, height, 1);

    m_frame->width = width;
    m_frame->height = height;
    m_frame->format = AV_PIX_FMT_NV12;
    m_frame->pts = m_pts++;

    // Set color properties
    switch (m_config.color_space) {
        case ColorSpace::BT709:
            m_frame->colorspace = AVCOL_SPC_BT709;
            m_frame->color_primaries = AVCOL_PRI_BT709;
            m_frame->color_trc = AVCOL_TRC_BT709;
            break;
        case ColorSpace::BT2020:
            m_frame->colorspace = AVCOL_SPC_BT2020_NCL;
            m_frame->color_primaries = AVCOL_PRI_BT2020;
            m_frame->color_trc = AVCOL_TRC_SMPTE2084;
            break;
    }

    // Send the frame to the encoder
    int ret = avcodec_send_frame(m_codec_ctx, m_frame);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending frame to encoder: %s", errbuf);
        return false;
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
            LOG_ERROR("Error receiving packet from encoder: %s", errbuf);
            return false;
        }

        // Add to packet queue
        EncodedPacket encoded;
        encoded.data.resize(packet->size);
        memcpy(encoded.data.data(), packet->data, packet->size);
        encoded.pts = packet->pts;
        encoded.dts = packet->dts;
        encoded.duration = packet->duration;
        encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

        m_packet_queue.push_back(encoded);
        m_frames_encoded++;

        av_packet_free(&packet);
    }

    return true;
}

bool VideoEncoder::get_encoded_packet(EncodedPacket& packet) {
    if (m_packet_read_index >= m_packet_queue.size()) {
        return false;
    }

    packet = m_packet_queue[m_packet_read_index++];
    return true;
}

void VideoEncoder::flush() {
    if (!m_initialized || !m_codec_ctx) return;

    // Send null frame to flush encoder
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
            encoded.dts = packet->dts;
            encoded.duration = packet->duration;
            encoded.keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

            m_packet_queue.push_back(encoded);
            av_packet_free(&packet);
        }
    }
}

// ─── Color Conversion ───────────────────────────────────────────────────────

bool VideoEncoder::convert_to_nv12(const VideoFrame& in_frame, uint8_t*& out_nv12, int& out_size) {
    int width = in_frame.width;
    int height = in_frame.height;

    // NV12 size = width * height (Y) + width * height / 2 (UV interleaved)
    out_size = width * height + width * height / 2;
    out_nv12 = static_cast<uint8_t*>(av_malloc(out_size));
    if (!out_nv12) {
        LOG_ERROR("Failed to allocate NV12 buffer");
        return false;
    }

    // Create or reuse sws context
    m_sws_ctx = sws_getCachedContext(
        m_sws_ctx,
        width, height, AV_PIX_FMT_BGRA,
        m_codec_ctx->width, m_codec_ctx->height, AV_PIX_FMT_NV12,
        SWS_BILINEAR | SWS_ACCURATE_RND,
        nullptr, nullptr, nullptr
    );

    if (!m_sws_ctx) {
        LOG_ERROR("Failed to create sws context for BGRA->NV12 conversion");
        av_free(out_nv12);
        out_nv12 = nullptr;
        return false;
    }

    // Prepare source frame data
    uint8_t* src_data[1] = { in_frame.data };
    int src_linesize[1] = { in_frame.stride };

    // Prepare destination frame data
    uint8_t* dst_data[2] = { out_nv12, out_nv12 + width * height };
    int dst_linesize[2] = { width, width };

    // Perform conversion
    int ret = sws_scale(m_sws_ctx, src_data, src_linesize, 0, height, dst_data, dst_linesize);
    if (ret < 0) {
        LOG_ERROR("sws_scale failed: %d", ret);
        av_free(out_nv12);
        out_nv12 = nullptr;
        return false;
    }

    return true;
}

// ─── Helpers ────────────────────────────────────────────────────────────────

AVFrame* VideoEncoder::allocate_frame(int width, int height, int format) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->width = width;
    frame->height = height;
    frame->format = format;

    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    return frame;
}

std::string VideoEncoder::encoder_name() const {
    if (m_codec) return m_codec->name;
    return "none";
}
