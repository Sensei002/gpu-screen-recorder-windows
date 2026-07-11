#include "encode/direct_nvenc.h"
#include "common/log.h"

// Must include Windows headers before ffnvcodec for GUID and calling convention
#include <windows.h>
#include <d3d11.h>

// Include the official NVENC API header (from nv-codec-headers, API v13.1)
#include <ffnvcodec/nvEncodeAPI.h>

#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>

// Helper: cast the opaque m_funcs pointer to the real NVENC function list type.
// m_funcs is stored as void* in the header to avoid including nvEncodeAPI.h there.
#define NVF() ((NV_ENCODE_API_FUNCTION_LIST*)m_funcs)

// ─── Struct version helpers ─────────────────────────────────────────────────
// The NVENC struct version field embeds the API version in bits 0-7 (major)
// and 24-27 (minor). When we negotiate an older API version, we must adjust
// the struct version fields to match, otherwise the driver returns
// NV_ENC_ERR_INVALID_VERSION.
//
// NVENCAPI_STRUCT_VERSION(n) = NVENCAPI_VERSION | (n << 16) | (0x7 << 28)
// Some structs also have bit 31 set (extended flag).

static inline uint32_t nv_struct_ver(uint32_t api_ver, uint32_t struct_n) {
    return api_ver | (struct_n << 16) | (0x7u << 28);
}
static inline uint32_t nv_struct_ver_ext(uint32_t api_ver, uint32_t struct_n) {
    return api_ver | (struct_n << 16) | (0x7u << 28) | (1u << 31);
}

// ═════════════════════════════════════════════════════════════════════════════
//  Implementation
// ═════════════════════════════════════════════════════════════════════════════

DirectNVENC::DirectNVENC() = default;

DirectNVENC::~DirectNVENC() {
    shutdown();
}

// ─── Static: Check availability ────────────────────────────────────────────

bool DirectNVENC::is_available() {
    HMODULE mod = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!mod) mod = LoadLibraryW(L"nvEncodeAPI.dll");
    if (!mod) return false;
    FreeLibrary(mod);
    return true;
}

// ─── Load library ──────────────────────────────────────────────────────────

bool DirectNVENC::load_library() {
    if (m_module) return true;

    m_module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_module) m_module = LoadLibraryW(L"nvEncodeAPI.dll");
    if (!m_module) {
        LOG_ERROR("DirectNVENC: Failed to load nvEncodeAPI.dll (error %lu)", GetLastError());
        return false;
    }

    // Get NvEncodeAPICreateInstance entry point
    typedef NVENCSTATUS (NVENCAPI* NvEncodeAPICreateInstance_t)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = (NvEncodeAPICreateInstance_t)
        GetProcAddress((HMODULE)m_module, "NvEncodeAPICreateInstance");
    if (!createInstance) {
        LOG_ERROR("DirectNVENC: NvEncodeAPICreateInstance not found");
        unload_library();
        return false;
    }

    // Allocate function list (use latest struct from header; SDK fills what it can)
    NV_ENCODE_API_FUNCTION_LIST* fl =
        (NV_ENCODE_API_FUNCTION_LIST*)calloc(1, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    m_funcs = fl;
    if (!fl) {
        LOG_ERROR("DirectNVENC: Failed to allocate function list");
        unload_library();
        return false;
    }
    fl->version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = createInstance(fl);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: NvEncodeAPICreateInstance failed with status %u", status);
        free(m_funcs);
        m_funcs = nullptr;
        unload_library();
        return false;
    }

    // Verify critical functions (use the stored pointer via NVF())
    if (!NVF()->nvEncOpenEncodeSessionEx) {
        LOG_ERROR("DirectNVENC: nvEncOpenEncodeSessionEx is null");
        free(NVF()); m_funcs = nullptr;
        unload_library();
        return false;
    }
    if (!NVF()->nvEncInitializeEncoder) {
        LOG_ERROR("DirectNVENC: nvEncInitializeEncoder is null");
        free(NVF()); m_funcs = nullptr;
        unload_library();
        return false;
    }
    if (!NVF()->nvEncEncodePicture) {
        LOG_ERROR("DirectNVENC: nvEncEncodePicture is null");
        free(NVF()); m_funcs = nullptr;
        unload_library();
        return false;
    }
    if (!NVF()->nvEncDestroyEncoder) {
        LOG_ERROR("DirectNVENC: nvEncDestroyEncoder is null");
        free(NVF()); m_funcs = nullptr;
        unload_library();
        return false;
    }

    LOG_INFO("DirectNVENC: Loaded nvEncodeAPI.dll (funcs ver=%u)", NVF()->version);
    return true;
}

void DirectNVENC::unload_library() {
    if (m_funcs) { free(NVF()); m_funcs = nullptr; }
    if (m_module) { FreeLibrary((HMODULE)m_module); m_module = nullptr; }
}

// ─── Create encode session ─────────────────────────────────────────────────
// Try different API versions until one succeeds. Maxwell GPUs (GTX 9xx)
// support up to NVENC API v6–v7. Newer GPUs support up to v13+.
// The apiVersion field format is: major | (minor << 24), so plain integers
// like 7u = API v7.0, 12u = API v12.0, etc.

bool DirectNVENC::create_session() {
    if (!m_funcs) return false;

    // API versions to try, newest first. Maxwell supports ~v6–v7.
    // NVENCAPI_VERSION = v13.1 (from our header).
    const uint32_t api_versions[] = {
        NVENCAPI_VERSION,  // v13.1 — latest (from ffnvcodec header)
        12u,               // v12.x
        11u,               // v11.x
        10u,               // v10.x
        9u,                // v9.x
        8u,                // v8.x
        7u,                // v7.x — Maxwell max
        6u                 // v6.x — also should work on Maxwell
    };

    NVENCSTATUS last_status = NV_ENC_ERR_INVALID_VERSION;

    for (uint32_t ver : api_versions) {
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
        memset(&params, 0, sizeof(params));
        params.version    = nv_struct_ver(ver, 1);  // struct 1 = OPEN_ENCODE_SESSION_EX_PARAMS
        params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        params.device     = m_device;
        params.apiVersion = ver;

        NVENCSTATUS status = NVF()->nvEncOpenEncodeSessionEx(&params, &m_session);
        if (status == NV_ENC_SUCCESS) {
            m_api_version = ver;
            LOG_INFO("DirectNVENC: Session opened with API v%u", ver);
            return true;
        }

        last_status = status;
        if (status != NV_ENC_ERR_INVALID_VERSION) {
            // Non-version error — no point trying older versions
            LOG_ERROR("DirectNVENC: nvEncOpenEncodeSessionEx failed with status %u (ver=%u)",
                      status, ver);
            m_session = nullptr;
            return false;
        }
    }

    LOG_ERROR("DirectNVENC: nvEncOpenEncodeSessionEx failed — no compatible API version found (last status %u)",
              last_status);
    m_session = nullptr;
    return false;
}

// ─── Configure encoder ─────────────────────────────────────────────────────

bool DirectNVENC::configure_encoder() {
    if (!m_funcs || !m_session) return false;

    // ── H.264 specific config ──────────────────────────────────────────
    NV_ENC_CONFIG_H264 h264Config{};
    memset(&h264Config, 0, sizeof(h264Config));
    // All bitfields are already 0 (no temporal SVC, no MVC, no hier P/B, etc.)
    h264Config.level              = NV_ENC_LEVEL_AUTOSELECT;
    h264Config.idrPeriod          = (uint32_t)(m_fps * 2);
    h264Config.separateColourPlaneFlag = 0;
    h264Config.disableDeblockingFilterIDC = 0;
    h264Config.entropyCodingMode  = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    h264Config.stereoMode         = NV_ENC_STEREO_PACKING_MODE_NONE;
    h264Config.adaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_AUTOSELECT;
    h264Config.fmoMode            = NV_ENC_H264_FMO_DISABLE;
    h264Config.bdirectMode        = NV_ENC_H264_BDIRECT_MODE_AUTOSELECT;
    h264Config.outputPictureTimingSEI = 0;
    h264Config.outputAUD          = 0;

    // ── Main codec config ──────────────────────────────────────────────
    NV_ENC_CONFIG encConfig{};
    memset(&encConfig, 0, sizeof(encConfig));
    encConfig.version        = nv_struct_ver_ext(m_api_version, 9);  // struct 9 = NV_ENC_CONFIG
    encConfig.profileGUID    = NV_ENC_H264_PROFILE_MAIN_GUID;
    encConfig.gopLength      = (uint32_t)(m_fps * 2);   // 2-second GOP
    encConfig.frameIntervalP = 1;                        // I and P only (no B-frames)
    encConfig.mvPrecision    = NV_ENC_MV_PRECISION_DEFAULT;
    encConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    // ── Rate control ───────────────────────────────────────────────────
    if (m_constant_quality) {
        encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encConfig.rcParams.targetQuality   = (uint8_t)m_quality;
        encConfig.rcParams.targetQualityLSB = 0;
        encConfig.rcParams.averageBitRate  = (uint32_t)(m_bitrate_kbps * 1000);
        encConfig.rcParams.maxBitRate      = (uint32_t)(m_bitrate_kbps * 1000 * 2);
        encConfig.rcParams.vbvBufferSize   = (uint32_t)(m_bitrate_kbps * 1000);
        encConfig.rcParams.vbvInitialDelay = (uint32_t)(m_bitrate_kbps * 1000 / 2);
    } else {
        encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        encConfig.rcParams.averageBitRate  = (uint32_t)(m_bitrate_kbps * 1000);
        encConfig.rcParams.maxBitRate      = (uint32_t)(m_bitrate_kbps * 1000);
        encConfig.rcParams.vbvBufferSize   = (uint32_t)(m_bitrate_kbps * 1000);
        encConfig.rcParams.vbvInitialDelay = (uint32_t)(m_bitrate_kbps * 1000 / 2);
    }

    // Set H.264 specific config through the union
    encConfig.encodeCodecConfig.h264Config = h264Config;

    // ── Initialize params ──────────────────────────────────────────────
    NV_ENC_INITIALIZE_PARAMS initParams{};
    memset(&initParams, 0, sizeof(initParams));
    initParams.version        = nv_struct_ver_ext(m_api_version, 7);  // struct 7 = NV_ENC_INITIALIZE_PARAMS
    initParams.encodeGUID     = NV_ENC_CODEC_H264_GUID;
    initParams.presetGUID     = NV_ENC_PRESET_P3_GUID;  // P3 = efficient, widely compatible
    initParams.encodeWidth    = (uint32_t)m_width;
    initParams.encodeHeight   = (uint32_t)m_height;
    initParams.darWidth       = (uint32_t)m_width;
    initParams.darHeight      = (uint32_t)m_height;
    initParams.frameRateNum   = (uint32_t)m_fps;
    initParams.frameRateDen   = 1;
    initParams.enableEncodeAsync = 0;   // synchronous mode
    initParams.enablePTD      = 1;       // let NVENC decide picture types
    initParams.encodeConfig   = &encConfig;  // use our custom config
    initParams.maxEncodeWidth = (uint32_t)m_width;
    initParams.maxEncodeHeight = (uint32_t)m_height;

    NVENCSTATUS status = NVF()->nvEncInitializeEncoder(m_session, &initParams);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncInitializeEncoder failed with status %u", status);
        return false;
    }

    LOG_INFO("DirectNVENC: Encoder initialized: %dx%d @ %dfps, %s, quality=%d, bitrate=%d kbps",
             m_width, m_height, m_fps,
             m_constant_quality ? "CQ (VBR)" : "CBR",
             m_quality, m_bitrate_kbps);
    return true;
}

// ─── Create input/output buffers ───────────────────────────────────────────

bool DirectNVENC::create_buffers() {
    if (!m_funcs || !m_session) return false;

    // Create input buffer (system memory, NV12)
    NV_ENC_CREATE_INPUT_BUFFER inBuf{};
    memset(&inBuf, 0, sizeof(inBuf));
    inBuf.version   = nv_struct_ver(m_api_version, 2);  // struct 2 = NV_ENC_CREATE_INPUT_BUFFER
    inBuf.width     = (uint32_t)m_width;
    inBuf.height    = (uint32_t)m_height;
    inBuf.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS status = NVF()->nvEncCreateInputBuffer(m_session, &inBuf);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncCreateInputBuffer failed with status %u", status);
        return false;
    }
    m_input_buffer = inBuf.inputBuffer;
    LOG_DEBUG("DirectNVENC: Input buffer created");

    // Create output bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER outBuf{};
    memset(&outBuf, 0, sizeof(outBuf));
    outBuf.version = nv_struct_ver(m_api_version, 1);  // struct 1 = NV_ENC_CREATE_BITSTREAM_BUFFER

    status = NVF()->nvEncCreateBitstreamBuffer(m_session, &outBuf);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncCreateBitstreamBuffer failed with status %u", status);
        NVF()->nvEncDestroyInputBuffer(m_session, m_input_buffer);
        m_input_buffer = nullptr;
        return false;
    }
    m_output_buffer = outBuf.bitstreamBuffer;
    LOG_DEBUG("DirectNVENC: Bitstream buffer created");
    return true;
}

// ─── Get sequence params (SPS/PPS) ─────────────────────────────────────────

bool DirectNVENC::get_sequence_params() {
    if (!m_funcs || !m_session) return false;

    // Query size first
    uint8_t buffer[512];
    uint32_t actualSize = 0;

    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    memset(&payload, 0, sizeof(payload));
    payload.version        = nv_struct_ver(m_api_version, 1);  // struct 1 = NV_ENC_SEQUENCE_PARAM_PAYLOAD
    payload.inBufferSize   = sizeof(buffer);
    payload.spsppsBuffer   = buffer;
    payload.spsId          = 0;
    payload.ppsId          = 0;
    payload.outSPSPPSPayloadSize = &actualSize;

    NVENCSTATUS status = NVF()->nvEncGetSequenceParams(m_session, &payload);
    if (status != NV_ENC_SUCCESS || actualSize == 0) {
        LOG_WARN("DirectNVENC: Failed to get sequence params (status %u)", status);
        m_extradata.clear();
        return false;
    }

    m_extradata.assign(buffer, buffer + actualSize);
    LOG_INFO("DirectNVENC: Got sequence params (%u bytes)", actualSize);
    return true;
}

// ─── Initialize ────────────────────────────────────────────────────────────

bool DirectNVENC::initialize(ID3D11Device* d3d11_device,
                             int width, int height, int fps,
                             int quality, bool constant_quality, int bitrate_kbps) {
    if (!d3d11_device) {
        LOG_ERROR("DirectNVENC: D3D11 device is null");
        return false;
    }

    m_device = d3d11_device;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_quality = quality;
    m_constant_quality = constant_quality;
    m_bitrate_kbps = bitrate_kbps;

    if (!load_library())     { LOG_ERROR("DirectNVENC: Failed to load NVENC library"); return false; }
    if (!create_session())   { LOG_ERROR("DirectNVENC: Failed to create encode session"); shutdown(); return false; }
    if (!configure_encoder()){ LOG_ERROR("DirectNVENC: Failed to configure encoder"); shutdown(); return false; }
    if (!create_buffers())   { LOG_ERROR("DirectNVENC: Failed to create buffers"); shutdown(); return false; }

    get_sequence_params();

    LOG_INFO("DirectNVENC: Initialized: %dx%d @ %dfps", m_width, m_height, m_fps);
    return true;
}

// ─── Shutdown ──────────────────────────────────────────────────────────────

void DirectNVENC::shutdown() {
    if (m_funcs && m_session) {
        if (m_output_buffer) NVF()->nvEncDestroyBitstreamBuffer(m_session, m_output_buffer);
        m_output_buffer = nullptr;
        if (m_input_buffer) NVF()->nvEncDestroyInputBuffer(m_session, m_input_buffer);
        m_input_buffer = nullptr;
        NVF()->nvEncDestroyEncoder(m_session);
    }
    m_session = nullptr;
    m_device = nullptr;
    m_api_version = 0;

    m_packet_queue.clear();
    m_packet_read_index = 0;
    m_extradata.clear();
    m_frames_encoded = 0;
    m_pts_counter = 0;
    m_first_frame = true;

    unload_library();
    LOG_DEBUG("DirectNVENC: Shutdown");
}

// ─── Encode frame ──────────────────────────────────────────────────────────

bool DirectNVENC::encode_frame(const uint8_t* nv12_data, int width, int height,
                               int64_t timestamp_us) {
    if (!m_funcs || !m_session || !m_input_buffer || !m_output_buffer) return false;

    // Copy NV12 data directly to the system-memory input buffer.
    // nvEncCreateInputBuffer returns a system memory buffer; the handle
    // (NV_ENC_INPUT_PTR) IS the writable memory address.
    size_t y_size = (size_t)m_width * m_height;
    memcpy((uint8_t*)m_input_buffer, nv12_data, y_size);

    size_t uv_offset = y_size;
    size_t uv_size = (size_t)m_width * m_height / 2;
    memcpy((uint8_t*)m_input_buffer + uv_offset, nv12_data + uv_offset, uv_size);

    // ── Encode picture ─────────────────────────────────────────────────
    NV_ENC_PIC_PARAMS picParams{};
    memset(&picParams, 0, sizeof(picParams));
    picParams.version         = nv_struct_ver_ext(m_api_version, 7);  // struct 7 = NV_ENC_PIC_PARAMS
    picParams.inputWidth      = (uint32_t)m_width;
    picParams.inputHeight     = (uint32_t)m_height;
    picParams.inputPitch      = (uint32_t)m_width;
    picParams.inputBuffer     = m_input_buffer;
    picParams.outputBitstream = m_output_buffer;
    picParams.inputTimeStamp  = m_pts_counter;
    picParams.inputDuration   = 1;
    picParams.encodePicFlags  = 0;

    // Force IDR on first frame and every 2 seconds
    if (m_first_frame || (m_frames_encoded % (m_fps * 2) == 0)) {
        picParams.encodePicFlags |= NV_ENC_PIC_FLAG_FORCEIDR;
        picParams.encodePicFlags |= NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        m_first_frame = false;
    }

    NVENCSTATUS status = NVF()->nvEncEncodePicture(m_session, &picParams);
    if (status != NV_ENC_SUCCESS) {
        // NV_ENC_ERR_NEED_MORE_INPUT means the frame was buffered for re-ordering
        // (e.g. when B-frames are used). With frameIntervalP=1 this shouldn't happen,
        // but if it does, the frame IS buffered successfully — return true.
        if (status == NV_ENC_ERR_NEED_MORE_INPUT) {
            m_frames_encoded++;
            m_pts_counter++;
            return true;
        }
        LOG_ERROR("DirectNVENC: nvEncEncodePicture failed with status %u", status);
        return false;
    }

    // ── Retrieve encoded bitstream ─────────────────────────────────────
    NV_ENC_LOCK_BITSTREAM lockBistream{};
    memset(&lockBistream, 0, sizeof(lockBistream));
    lockBistream.version       = nv_struct_ver_ext(m_api_version, 2);  // struct 2 = NV_ENC_LOCK_BITSTREAM
    lockBistream.outputBitstream = m_output_buffer;
    lockBistream.doNotWait     = 0;  // synchronous

    status = NVF()->nvEncLockBitstream(m_session, &lockBistream);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncLockBitstream failed with status %u", status);
        return false;
    }

    // Copy encoded data
    EncodedPacket packet;
    packet.data.resize(lockBistream.bitstreamSizeInBytes);
    if (lockBistream.bitstreamSizeInBytes > 0 && lockBistream.bitstreamBufferPtr) {
        memcpy(packet.data.data(), lockBistream.bitstreamBufferPtr,
               lockBistream.bitstreamSizeInBytes);
    }
    packet.pts = m_pts_counter;
    packet.dts = m_pts_counter;
    packet.duration = 1;
    packet.keyframe = (lockBistream.pictureType == NV_ENC_PIC_TYPE_I ||
                       lockBistream.pictureType == NV_ENC_PIC_TYPE_IDR);

    m_packet_queue.push_back(std::move(packet));

    NVF()->nvEncUnlockBitstream(m_session, m_output_buffer);

    m_frames_encoded++;
    m_pts_counter++;
    return true;
}

// ─── Flush ─────────────────────────────────────────────────────────────────

void DirectNVENC::flush() {
    if (!m_funcs || !m_session) return;

    // Send EOS (End Of Stream)
    NV_ENC_PIC_PARAMS picParams{};
    memset(&picParams, 0, sizeof(picParams));
    picParams.version        = nv_struct_ver_ext(m_api_version, 7);  // struct 7 = NV_ENC_PIC_PARAMS
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    NVF()->nvEncEncodePicture(m_session, &picParams);

    // nvEncFlushEncoderQueue is not available in NVENC API v13+
    // EOS signal above is sufficient to flush the encoder

    LOG_DEBUG("DirectNVENC: Flushed (%lld frames)", (long long)m_frames_encoded);
}

// ─── Get next packet ───────────────────────────────────────────────────────

bool DirectNVENC::get_encoded_packet(EncodedPacket& packet) {
    if (m_packet_read_index >= m_packet_queue.size()) return false;
    packet = m_packet_queue[m_packet_read_index++];
    return true;
}
