#include "encode/direct_nvenc.h"
#include "common/log.h"

// Must include Windows headers before ffnvcodec for GUID and calling convention
#include <windows.h>
#include <d3d11.h>

// Include the official NVENC API header (from nv-codec-headers)
#include <ffnvcodec/nvEncodeAPI.h>

#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>

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
    if (!mod) {
        mod = LoadLibraryW(L"nvEncodeAPI.dll");
    }
    if (!mod) return false;
    FreeLibrary(mod);
    return true;
}

// ─── Load library ──────────────────────────────────────────────────────────

bool DirectNVENC::load_library() {
    if (m_module) return true;

    m_module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!m_module) {
        m_module = LoadLibraryW(L"nvEncodeAPI.dll");
    }
    if (!m_module) {
        LOG_ERROR("DirectNVENC: Failed to load nvEncodeAPI.dll (error %lu)", GetLastError());
        return false;
    }

    // Get the NvEncodeAPICreateInstance entry point
    typedef NVENCSTATUS (NVENCAPI* NvEncodeAPICreateInstance_t)(NV_ENCODE_API_FUNCTION_LIST*);
    auto createInstance = (NvEncodeAPICreateInstance_t)
        GetProcAddress((HMODULE)m_module, "NvEncodeAPICreateInstance");

    if (!createInstance) {
        LOG_ERROR("DirectNVENC: NvEncodeAPICreateInstance not found in nvEncodeAPI.dll");
        unload_library();
        return false;
    }

    // Allocate and initialize function list
    // We use a buffer of the size expected by API v7. The header declares the
    // struct for the latest API, but the runtime function list will be smaller.
    // We allocate the header's struct size and zero it — the SDK only fills
    // the function pointers it knows about.
    m_funcs = (NV_ENCODE_API_FUNCTION_LIST*)calloc(1, sizeof(NV_ENCODE_API_FUNCTION_LIST));
    if (!m_funcs) {
        LOG_ERROR("DirectNVENC: Failed to allocate function list");
        unload_library();
        return false;
    }
    m_funcs->version = NV_ENCODE_API_FUNCTION_LIST_VER;
    m_funcs->size = sizeof(NV_ENCODE_API_FUNCTION_LIST);

    NVENCSTATUS status = createInstance(m_funcs);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: NvEncodeAPICreateInstance failed with status %u", status);
        free(m_funcs);
        m_funcs = nullptr;
        unload_library();
        return false;
    }

    // Verify critical functions
    if (!m_funcs->nvEncOpenEncodeSessionEx) {
        LOG_ERROR("DirectNVENC: nvEncOpenEncodeSessionEx is null (driver too old?)");
        free(m_funcs);
        m_funcs = nullptr;
        unload_library();
        return false;
    }
    if (!m_funcs->nvEncInitializeEncoder) {
        LOG_ERROR("DirectNVENC: nvEncInitializeEncoder is null");
        free(m_funcs);
        m_funcs = nullptr;
        unload_library();
        return false;
    }
    if (!m_funcs->nvEncEncodePicture) {
        LOG_ERROR("DirectNVENC: nvEncEncodePicture is null");
        free(m_funcs);
        m_funcs = nullptr;
        unload_library();
        return false;
    }
    if (!m_funcs->nvEncDestroyEncoder) {
        LOG_ERROR("DirectNVENC: nvEncDestroyEncoder is null");
        free(m_funcs);
        m_funcs = nullptr;
        unload_library();
        return false;
    }

    LOG_INFO("DirectNVENC: Loaded nvEncodeAPI.dll successfully (funcs ver=%u, size=%u)",
             m_funcs->version, m_funcs->size);
    return true;
}

void DirectNVENC::unload_library() {
    if (m_funcs) {
        free(m_funcs);
        m_funcs = nullptr;
    }
    if (m_module) {
        FreeLibrary((HMODULE)m_module);
        m_module = nullptr;
    }
}

// ─── Create session ────────────────────────────────────────────────────────

bool DirectNVENC::create_session() {
    if (!m_funcs) return false;

    // Query the API version supported by the runtime
    uint32_t minVer = (7 << 4) | 0;   // We want API v7 minimum
    uint32_t maxVer = (12 << 4) | 0;  // Up to API v12

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
    memset(&params, 0, sizeof(params));
    params.version     = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.structSize  = sizeof(params);
    params.device      = m_device;
    params.deviceType  = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.apiVersion  = maxVer;  // Ask for latest, driver negotiates

    NVENCSTATUS status = m_funcs->nvEncOpenEncodeSessionEx(&params, &m_session);
    if (status != NV_ENC_SUCCESS) {
        // Try with lower API version
        params.apiVersion = minVer;
        status = m_funcs->nvEncOpenEncodeSessionEx(&params, &m_session);
        if (status != NV_ENC_SUCCESS) {
            LOG_ERROR("DirectNVENC: nvEncOpenEncodeSessionEx failed with status %u", status);
            m_session = nullptr;
            return false;
        }
    }

    LOG_INFO("DirectNVENC: Session opened successfully");
    return true;
}

// ─── Configure encoder ─────────────────────────────────────────────────────

bool DirectNVENC::configure_encoder() {
    if (!m_funcs || !m_session) return false;

    // ── H.264 specific config ──────────────────────────────────────────
    NV_ENC_CONFIG_H264 h264Config{};
    memset(&h264Config, 0, sizeof(h264Config));
    h264Config.version                = NV_ENC_CONFIG_H264_VER;
    h264Config.structSize             = sizeof(h264Config);
    h264Config.level                  = 0;   // auto-select
    h264Config.stereoMode             = 0;
    h264Config.entropyCodingMode      = NV_ENC_H264_ENTROPY_CODING_MODE_CABAC;
    h264Config.h264Profile            = NV_ENC_H264_PROFILE_MAIN;
    h264Config.h264PictureTimingSEI   = 0;
    h264Config.maxNumRefFrames        = 0;   // auto
    h264Config.h264AdaptiveTransformMode = NV_ENC_H264_ADAPTIVE_TRANSFORM_MODE_AUTO;
    h264Config.h264FMOEnabled         = 0;
    h264Config.enableFillerDataInsertion = 0;

    // ── Main codec config ──────────────────────────────────────────────
    NV_ENC_CONFIG encConfig{};
    memset(&encConfig, 0, sizeof(encConfig));
    encConfig.version        = NV_ENC_CONFIG_VER;
    encConfig.structSize     = sizeof(encConfig);
    encConfig.profileGUID    = NV_ENC_H264_PROFILE_MAIN_GUID;
    encConfig.gopLength      = m_fps * 2;    // 2-second GOP
    encConfig.frameIntervalP = 1;             // No B-frames (I and P only)
    encConfig.mvPrecision    = NV_ENC_MV_PRECISION_DEFAULT;
    encConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;

    // ── Rate control ───────────────────────────────────────────────────
    if (m_constant_quality) {
        // VBR with target quality (works on Maxwell+)
        encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
        encConfig.rcParams.targetQuality   = (uint32_t)m_quality;
        encConfig.rcParams.targetQualityLSB = 0;
        encConfig.rcParams.averageBitRate  = m_bitrate_kbps * 1000;
        encConfig.rcParams.maxBitRate      = m_bitrate_kbps * 1000 * 2;
        encConfig.rcParams.vbvBufferSize   = m_bitrate_kbps * 1000;
        encConfig.rcParams.vbvInitialDelay = m_bitrate_kbps * 1000 / 2;
    } else {
        // CBR mode
        encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        encConfig.rcParams.averageBitRate  = m_bitrate_kbps * 1000;
        encConfig.rcParams.maxBitRate      = m_bitrate_kbps * 1000;
        encConfig.rcParams.vbvBufferSize   = m_bitrate_kbps * 1000;
        encConfig.rcParams.vbvInitialDelay = m_bitrate_kbps * 1000 / 2;
    }

    encConfig.configSpecific = &h264Config;

    // ── Initialize params ──────────────────────────────────────────────
    NV_ENC_INITIALIZE_PARAMS initParams{};
    memset(&initParams, 0, sizeof(initParams));
    initParams.version        = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.structSize     = sizeof(initParams);
    initParams.encodeGUID     = NV_CODEC_H264_GUID;
    initParams.presetGUID     = NV_ENC_PRESET_HQ_GUID;
    initParams.encodeWidth    = (uint32_t)m_width;
    initParams.encodeHeight   = (uint32_t)m_height;
    initParams.darWidth       = (uint32_t)m_width;
    initParams.darHeight      = (uint32_t)m_height;
    initParams.frameRateNum   = (uint32_t)m_fps;
    initParams.frameRateDen   = 1;
    initParams.enableEncodeAsync = 0;  // synchronous
    initParams.frameIntervalP = 1;    // P-frame period (1 = no B-frames)
    initParams.maxEncodeWidth = (uint32_t)m_width;
    initParams.maxEncodeHeight = (uint32_t)m_height;
    initParams.presetConfig   = nullptr;
    initParams.encodeConfig   = &encConfig;
    initParams.avgBitRate     = encConfig.rcParams.averageBitRate;
    initParams.maxBitRate     = encConfig.rcParams.maxBitRate;

    NVENCSTATUS status = m_funcs->nvEncInitializeEncoder(m_session, &initParams);
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
    inBuf.version    = NV_ENC_CREATE_INPUT_BUFFER_VER;
    inBuf.structSize = sizeof(inBuf);
    inBuf.width      = (uint32_t)m_width;
    inBuf.height     = (uint32_t)m_height;
    inBuf.bufferFmt  = NV_ENC_BUFFER_FORMAT_NV12;

    NVENCSTATUS status = m_funcs->nvEncCreateInputBuffer(m_session, &inBuf);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncCreateInputBuffer failed with status %u", status);
        return false;
    }
    m_input_buffer = inBuf.inputBuffer;
    m_input_pitch  = inBuf.pitch;
    LOG_DEBUG("DirectNVENC: Input buffer created (pitch=%u)", inBuf.pitch);

    // Create output bitstream buffer
    NV_ENC_CREATE_BITSTREAM_BUFFER outBuf{};
    memset(&outBuf, 0, sizeof(outBuf));
    outBuf.version    = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    outBuf.structSize = sizeof(outBuf);
    outBuf.size       = 4 * 1024 * 1024;  // 4 MB

    status = m_funcs->nvEncCreateBitstreamBuffer(m_session, &outBuf);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncCreateBitstreamBuffer failed with status %u", status);
        m_funcs->nvEncDestroyInputBuffer(m_session, m_input_buffer);
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
    NV_ENC_SEQUENCE_PARAM_PAYLOAD payload{};
    memset(&payload, 0, sizeof(payload));
    payload.version    = NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER;
    payload.structSize = sizeof(payload);
    payload.inBufferSize = 0;
    payload.spsId      = 0;
    payload.ppsId      = 0;

    // First call fails (buffer too small) but sets inBufferSize
    NVENCSTATUS status = m_funcs->nvEncGetSequenceParams(m_session, &payload);
    if (payload.inBufferSize == 0) {
        // Use a reasonable default
        payload.inBufferSize = 256;
    }

    m_extradata.resize(payload.inBufferSize);
    payload.buffer    = m_extradata.data();
    payload.bufferSize = (uint32_t)m_extradata.size();

    // Try with different SPS/PPS IDs if needed
    for (uint32_t spsId = 0; spsId < 2; spsId++) {
        for (uint32_t ppsId = 0; ppsId < 2; ppsId++) {
            payload.spsId = spsId;
            payload.ppsId = ppsId;
            status = m_funcs->nvEncGetSequenceParams(m_session, &payload);
            if (status == NV_ENC_SUCCESS && payload.cbSeqParamSize > 0) {
                m_extradata.resize(payload.cbSeqParamSize);
                LOG_INFO("DirectNVENC: Got sequence params (%u bytes, sps=%u, pps=%u)",
                         payload.cbSeqParamSize, spsId, ppsId);
                return true;
            }
        }
    }

    LOG_WARN("DirectNVENC: Failed to get sequence params (status %u)", status);
    m_extradata.clear();
    return false;
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

    // Step 1: Load nvEncodeAPI.dll
    if (!load_library()) {
        LOG_ERROR("DirectNVENC: Failed to load NVENC library");
        return false;
    }

    // Step 2: Create encode session
    if (!create_session()) {
        LOG_ERROR("DirectNVENC: Failed to create encode session");
        shutdown();
        return false;
    }

    // Step 3: Configure encoder (H.264, quality settings)
    if (!configure_encoder()) {
        LOG_ERROR("DirectNVENC: Failed to configure encoder");
        shutdown();
        return false;
    }

    // Step 4: Create input/output buffers
    if (!create_buffers()) {
        LOG_ERROR("DirectNVENC: Failed to create buffers");
        shutdown();
        return false;
    }

    // Step 5: Fetch sequence parameters (SPS/PPS for muxer)
    get_sequence_params();

    LOG_INFO("DirectNVENC: Initialized successfully: %dx%d @ %dfps",
             m_width, m_height, m_fps);
    return true;
}

// ─── Shutdown ──────────────────────────────────────────────────────────────

void DirectNVENC::shutdown() {
    // Destroy buffers
    if (m_funcs && m_session && m_output_buffer) {
        m_funcs->nvEncDestroyBitstreamBuffer(m_session, m_output_buffer);
    }
    m_output_buffer = nullptr;

    if (m_funcs && m_session && m_input_buffer) {
        m_funcs->nvEncDestroyInputBuffer(m_session, m_input_buffer);
    }
    m_input_buffer = nullptr;

    // Destroy encoder session
    if (m_funcs && m_session) {
        m_funcs->nvEncDestroyEncoder(m_session);
    }
    m_session = nullptr;
    m_device = nullptr;

    // Clear queues
    m_packet_queue.clear();
    m_packet_read_index = 0;
    m_extradata.clear();
    m_frames_encoded = 0;
    m_pts_counter = 0;
    m_first_frame = true;

    // Unload library
    unload_library();

    LOG_DEBUG("DirectNVENC: Shutdown complete");
}

// ─── Encode frame ──────────────────────────────────────────────────────────

bool DirectNVENC::encode_frame(const uint8_t* nv12_data, int width, int height,
                               int64_t timestamp_us) {
    if (!m_funcs || !m_session || !m_input_buffer || !m_output_buffer) {
        return false;
    }

    // The input buffer created by nvEncCreateInputBuffer is a system memory
    // buffer. NV_ENC_INPUT_PTR is just a handle, not always a direct pointer.
    // We need to map the input resource to get a writable pointer.
    // However, for system memory buffers created with nvEncCreateInputBuffer,
    // we can write to it through nvEncLockInputBuffer / nvEncUnlockInputBuffer.
    //
    // Since these lock/unlock functions exist in later API versions and we
    // might not have them, let's use nvEncMapInputResource which is more
    // universally available.
    //
    // Actually, the simplest approach: we copy NV12 data into the input buffer
    // directly. The NV_ENC_INPUT_PTR from nvEncCreateInputBuffer IS also the
    // memory address for system memory buffers.

    // Copy Y plane
    size_t y_size = (size_t)m_width * m_height;
    memcpy((uint8_t*)m_input_buffer, nv12_data, y_size);

    // Copy UV plane (interleaved)
    size_t uv_offset = y_size;
    size_t uv_size = (size_t)m_width * m_height / 2;
    memcpy((uint8_t*)m_input_buffer + uv_offset, nv12_data + uv_offset, uv_size);

    // ── Encode picture ─────────────────────────────────────────────────
    NV_ENC_PIC_PARAMS picParams{};
    memset(&picParams, 0, sizeof(picParams));
    picParams.version         = NV_ENC_PIC_PARAMS_VER;
    picParams.structSize      = sizeof(picParams);
    picParams.inputBuffer     = m_input_buffer;
    picParams.outputBitstream = m_output_buffer;
    picParams.inputWidth      = (uint16_t)m_width;
    picParams.inputHeight     = (uint16_t)m_height;
    picParams.inputPitch      = (uint16_t)m_width;  // NV12 Y pitch = width
    picParams.inputTimeStamp  = m_pts_counter;
    picParams.inputDuration   = 1;
    picParams.frameIdx        = (uint32_t)m_frames_encoded;

    // Force IDR on first frame and every 2 seconds
    if (m_first_frame || (m_frames_encoded % (m_fps * 2) == 0)) {
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_OUTPUT_IDR;
        m_first_frame = false;
    } else {
        picParams.encodePicFlags = 0;
    }

    NVENCSTATUS status = m_funcs->nvEncEncodePicture(m_session, &picParams);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncEncodePicture failed with status %u", status);
        return false;
    }

    // ── Retrieve encoded bitstream ─────────────────────────────────────
    NV_ENC_LOCK_BITSTREAM lockBistream{};
    memset(&lockBistream, 0, sizeof(lockBistream));
    lockBistream.version         = NV_ENC_LOCK_BITSTREAM_VER;
    lockBistream.structSize      = sizeof(lockBistream);
    lockBistream.outputBitstream = m_output_buffer;
    lockBistream.doNotWait       = 0;  // wait for encode to finish

    status = m_funcs->nvEncLockBitstream(m_session, &lockBistream);
    if (status != NV_ENC_SUCCESS) {
        LOG_ERROR("DirectNVENC: nvEncLockBitstream failed with status %u", status);
        // Bitstream buffer might be in an inconsistent state;
        // skip and continue — next frame will try again
        return false;
    }

    // Copy encoded data to packet
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

    // Unlock bitstream
    status = m_funcs->nvEncUnlockBitstream(m_session, m_output_buffer);
    if (status != NV_ENC_SUCCESS) {
        LOG_WARN("DirectNVENC: nvEncUnlockBitstream failed with status %u", status);
    }

    m_frames_encoded++;
    m_pts_counter++;
    return true;
}

// ─── Flush ─────────────────────────────────────────────────────────────────

void DirectNVENC::flush() {
    if (!m_funcs || !m_session) return;

    // Send EOS (End Of Stream) picture
    NV_ENC_PIC_PARAMS picParams{};
    memset(&picParams, 0, sizeof(picParams));
    picParams.version      = NV_ENC_PIC_PARAMS_VER;
    picParams.structSize   = sizeof(picParams);
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;

    m_funcs->nvEncEncodePicture(m_session, &picParams);

    // Flush encoder queue
    if (m_funcs->nvEncFlushEncoderQueue) {
        NV_ENC_FLUSH_PARAMS flushParams{};
        memset(&flushParams, 0, sizeof(flushParams));
        flushParams.version    = NV_ENC_FLUSH_PARAMS_VER;
        flushParams.structSize = sizeof(flushParams);
        m_funcs->nvEncFlushEncoderQueue(m_session, &flushParams);
    }

    LOG_DEBUG("DirectNVENC: Flushed encoder (%lld frames encoded)", (long long)m_frames_encoded);
}

// ─── Get next packet ───────────────────────────────────────────────────────

bool DirectNVENC::get_encoded_packet(EncodedPacket& packet) {
    if (m_packet_read_index >= m_packet_queue.size()) {
        return false;
    }
    packet = m_packet_queue[m_packet_read_index++];
    return true;
}
