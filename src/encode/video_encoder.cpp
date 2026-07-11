#include "encode/video_encoder.h"
#include "encode/direct_nvenc.h"
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
    m_d3d11_device = config.d3d11_device;
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
        // NOTE: AV_CODEC_FLAG_QSCALE is NOT set here because hardware encoders
        // (NVENC, AMF, QSV) don't support it — they use their own quality params
        // via av_opt_set. Setting QSCALE on NVENC causes avcodec_open2() to fail
        // with EINVAL on Maxwell-era GPUs and recent FFmpeg builds.

        // Set codec-specific quality options
        const char* quality_param = nullptr;
        if (strstr(m_codec->name, "nvenc")) {
            quality_param = "cq";
            av_opt_set(m_codec_ctx->priv_data, "rc", "vbr", 0);
        } else if (strstr(m_codec->name, "amf")) {
            quality_param = "quality";
        } else if (strstr(m_codec->name, "qsv")) {
            quality_param = "global_quality";
        } else if (strstr(m_codec->name, "libx264") || strstr(m_codec->name, "libx265")) {
            quality_param = "crf";
            // Software encoders DO support traditional QSCALE
            m_codec_ctx->flags |= AV_CODEC_FLAG_QSCALE;
            m_codec_ctx->global_quality = m_config.quality;
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
        // Use "hq" preset for broadest compatibility (Maxwell through Ada).
        // The "p6"/"p7" presets are Turing+ only and will fail on GTX 900-series (Maxwell).
        av_opt_set(m_codec_ctx->priv_data, "preset", "hq", 0);
        // Set profile explicitly — "main" is universally supported on all NVENC hardware.
        av_opt_set(m_codec_ctx->priv_data, "profile", "main", 0);
        // rc is set above in the quality section if constant_quality is enabled,
        // otherwise set it now for bitrate mode
        if (!m_config.constant_quality) {
            av_opt_set(m_codec_ctx->priv_data, "rc", "cbr", 0);
        }
        av_opt_set_int(m_codec_ctx->priv_data, "b_ref_mode", 0, 0); // No B-frames
        // NOTE: "delay" is intentionally NOT set. On Maxwell, some NVENC options
        // (like delay=0) can cause avcodec_open2() to reject the config.

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

    // For NVENC on Windows, create a hardware device context and share it with
    // the encoder. Try CUDA first (NVENC typically uses CUDA interop), then D3D11VA.
    // This is necessary because recent FFmpeg NVENC requires a hw context for
    // the encoder session, and some GPU/driver combos need one type over the other.
    AVBufferRef* hw_device_ctx = nullptr;
    if (strstr(m_codec->name, "nvenc")) {
        // Try CUDA first (NVENC is CUDA-based)
        int hw_ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA,
                                            nullptr, nullptr, 0);
        if (hw_ret >= 0) {
            m_codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
            LOG_INFO("Created CUDA hardware context for NVENC");
        } else {
            char hw_errbuf[256];
            av_strerror(hw_ret, hw_errbuf, sizeof(hw_errbuf));
            LOG_WARN("Could not create CUDA context, trying D3D11VA: %s", hw_errbuf);
            // Fall back to D3D11VA
            hw_ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_D3D11VA,
                                            nullptr, nullptr, 0);
            if (hw_ret >= 0) {
                m_codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
                LOG_INFO("Created D3D11VA hardware context for NVENC");
            } else {
                av_strerror(hw_ret, hw_errbuf, sizeof(hw_errbuf));
                LOG_WARN("Could not create D3D11VA context either: %s", hw_errbuf);
            }
        }
    }

    // Open the codec
    int ret = avcodec_open2(m_codec_ctx, m_codec, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to open encoder: %s", errbuf);
        if (hw_device_ctx) {
            av_buffer_unref(&hw_device_ctx);
        }
        
        // If this is an NVENC encoder that failed, try DirectNVENC instead
        // (bypassing FFmpeg's incompatible NVENC wrapper)
        if (strstr(m_codec->name, "nvenc")) {
            LOG_WARN("FFmpeg NVENC failed, trying DirectNVENC (OBS approach)...");
            return try_open_with_direct_nvenc();
        }
        
        return false;
    }
    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
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

bool VideoEncoder::try_open_with_direct_nvenc() {
    if (!m_d3d11_device) {
        LOG_ERROR("DirectNVENC: No D3D11 device available (set via config.d3d11_device)");
        return false;
    }
    
    // Clean up FFmpeg encoder context (it failed to open)
    if (m_codec_ctx) {
        avcodec_free_context(&m_codec_ctx);
        m_codec_ctx = nullptr;
    }
    
    // Create and initialize DirectNVENC
    m_direct_nvenc = std::make_unique<DirectNVENC>();
    
    bool ok = m_direct_nvenc->initialize(
        (ID3D11Device*)m_d3d11_device,
        m_config.width,
        m_config.height,
        m_config.fps,
        m_config.quality,
        m_config.constant_quality,
        m_config.bitrate_kbps
    );
    
    if (!ok) {
        LOG_ERROR("DirectNVENC: Failed to initialize");
        m_direct_nvenc.reset();
        return false;
    }
    
    // ── Create a synthetic AVCodecContext for the muxer ──────────────────
    // The muxer needs an AVCodecContext to write the file header (codec_id,
    // dimensions, extradata). We create one without calling avcodec_open2()
    // since DirectNVENC handles the actual encoding. The context is purely
    // metadata for the muxer.
    const AVCodec* h264_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    m_codec_ctx = avcodec_alloc_context3(h264_codec);
    if (!m_codec_ctx) {
        LOG_ERROR("DirectNVENC: Failed to allocate AVCodecContext for muxer");
        m_direct_nvenc->shutdown();
        m_direct_nvenc.reset();
        return false;
    }
    
    // Populate the context with our encoding parameters (no avcodec_open2!)
    m_codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    m_codec_ctx->codec_id = AV_CODEC_ID_H264;
    m_codec_ctx->width = m_config.width;
    m_codec_ctx->height = m_config.height;
    m_codec_ctx->time_base = AVRational{1, m_config.fps};
    m_codec_ctx->framerate = AVRational{m_config.fps, 1};
    m_codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
    m_codec_ctx->gop_size = m_config.fps * 2;
    m_codec_ctx->max_b_frames = 0;
    m_codec_ctx->bit_rate = m_config.bitrate_kbps * 1000;
    m_codec_ctx->profile = 77; // FF_PROFILE_H264_MAIN
    m_codec_ctx->level = 41;  // 4.1, supports 1080p@60
    
    // Copy extradata (SPS/PPS) from DirectNVENC to the FFmpeg context
    const uint8_t* extra = m_direct_nvenc->extra_data();
    int extra_size = m_direct_nvenc->extra_data_size();
    if (extra && extra_size > 0) {
        m_codec_ctx->extradata = (uint8_t*)av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (m_codec_ctx->extradata) {
            memcpy(m_codec_ctx->extradata, extra, extra_size);
            m_codec_ctx->extradata_size = extra_size;
        }
    }
    
    m_initialized = true;
    LOG_INFO("Encoder initialized: h264_nvenc (DirectNVENC, %s)",
             m_config.constant_quality ? "CQ mode" : "bitrate mode");
    return true;
}

void VideoEncoder::shutdown() {
    if (m_initialized) {
        flush();
    }

    // Shutdown DirectNVENC if active
    if (m_direct_nvenc) {
        m_direct_nvenc->shutdown();
        m_direct_nvenc.reset();
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

    // Free extradata (FFmpeg path)
    if (m_extradata) {
        av_free(m_extradata);
        m_extradata = nullptr;
    }

    // Free the FFmpeg codec context (handles both FFmpeg encoder path
    // and the synthetic metadata context created for DirectNVENC)
    if (m_codec_ctx) {
        avcodec_free_context(&m_codec_ctx);
    }

    // Clear packet queue
    m_packet_queue.clear();
    m_packet_read_index = 0;

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
    if (!m_initialized) return false;

    // Route to DirectNVENC if active
    if (m_direct_nvenc) {
        bool ok = m_direct_nvenc->encode_frame(nv12_data, width, height, timestamp_us);
        if (ok) {
            m_frames_encoded++;
        }
        return ok;
    }

    // FFmpeg path
    if (!m_codec_ctx) return false;

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
    // Try DirectNVENC queue first if active
    if (m_direct_nvenc) {
        // Drain DirectNVENC's internal queue into our shared queue
        DirectNVENC::EncodedPacket dp;
        while (m_direct_nvenc->get_encoded_packet(dp)) {
            EncodedPacket ep;
            ep.data = std::move(dp.data);
            ep.pts = dp.pts;
            ep.dts = dp.dts;
            ep.duration = dp.duration;
            ep.keyframe = dp.keyframe;
            m_packet_queue.push_back(std::move(ep));
        }
    }

    if (m_packet_read_index >= m_packet_queue.size()) {
        return false;
    }

    packet = m_packet_queue[m_packet_read_index++];
    return true;
}

void VideoEncoder::flush() {
    if (!m_initialized) return;

    // Flush DirectNVENC if active
    if (m_direct_nvenc) {
        m_direct_nvenc->flush();
        return;
    }

    // FFmpeg path
    if (!m_codec_ctx) return;

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
    int enc_width = m_config.width;
    int enc_height = m_config.height;

    // NV12 size = width * height (Y) + width * height / 2 (UV interleaved)
    out_size = width * height + width * height / 2;
    out_nv12 = static_cast<uint8_t*>(av_malloc(out_size));
    if (!out_nv12) {
        LOG_ERROR("Failed to allocate NV12 buffer");
        return false;
    }

    // Create or reuse sws context (use encoder dimensions from config)
    m_sws_ctx = sws_getCachedContext(
        m_sws_ctx,
        width, height, AV_PIX_FMT_BGRA,
        enc_width, enc_height, AV_PIX_FMT_NV12,
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
    if (m_direct_nvenc) return "h264_nvenc (direct)";
    if (m_codec) return m_codec->name;
    return "none";
}
