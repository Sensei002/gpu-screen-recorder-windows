#include "capture/audio_capture.h"
#include "common/log.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <windows.h>
#include <avrt.h>
#include <chrono>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "uuid.lib")

// Helper to convert WCHAR string to UTF-8 std::string without ATL dependency
static std::string wstring_to_utf8(const wchar_t* wstr) {
    if (!wstr) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &result[0], len, nullptr, nullptr);
    return result;
}

// ─── COM Smart Pointer Helper ───────────────────────────────────────────────

template<typename T>
class ComPtr {
public:
    ComPtr() : ptr(nullptr) {}
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** operator&() { return &ptr; }
    T* operator->() { return ptr; }
    T* get() const { return ptr; }
    operator bool() const { return ptr != nullptr; }
    T** reset() { if (ptr) { ptr->Release(); ptr = nullptr; } return &ptr; }
private:
    T* ptr = nullptr;
};

// ─── Constructor / Destructor ───────────────────────────────────────────────

AudioCapture::AudioCapture() = default;

AudioCapture::~AudioCapture() {
    shutdown();
}

// ─── List Audio Devices ─────────────────────────────────────────────────────

std::vector<AudioDeviceInfo> AudioCapture::list_devices() {
    std::vector<AudioDeviceInfo> devices;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool coinit = SUCCEEDED(hr);
    if (!coinit && hr != RPC_E_CHANGED_MODE) return devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        if (coinit) CoUninitialize();
        return devices;
    }

    // Enumerate render devices (outputs)
    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(collection->Item(i, &device))) {
                IPropertyStore* props = nullptr;
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    
                    AudioDeviceInfo info;
                    info.is_default_output = false;
                    info.is_default_input = false;

                    // Get device name
                    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var))) {
                        info.name = wstring_to_utf8(var.pwszVal);
                        PropVariantClear(&var);
                    }

                    if (SUCCEEDED(props->GetValue(PKEY_AudioEngine_DeviceFormat, &var))) {
                        PropVariantClear(&var);
                    }

                    // Get device ID
                    LPWSTR device_id = nullptr;
                    if (SUCCEEDED(device->GetId(&device_id))) {
                        info.id = wstring_to_utf8(device_id);
                        CoTaskMemFree(device_id);
                    }

                    info.default_sample_rate = 48000;
                    info.default_channels = 2;

                    devices.push_back(info);
                    props->Release();
                }
                device->Release();
            }
        }
        collection->Release();
    }

    // Get default device
    IMMDevice* default_device = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &default_device))) {
        LPWSTR default_id = nullptr;
        if (SUCCEEDED(default_device->GetId(&default_id))) {
            std::string default_id_str = wstring_to_utf8(default_id);
            for (auto& d : devices) {
                if (d.id == default_id_str) {
                    d.is_default_output = true;
                    break;
                }
            }
            CoTaskMemFree(default_id);
        }
        default_device->Release();
    }

    enumerator->Release();
    if (coinit) CoUninitialize();

    return devices;
}

std::vector<std::string> AudioCapture::list_application_audio() {
    // This would use IAudioSessionEnumerator to list running audio applications
    // For now, return an empty list (placeholder for future implementation)
    return {};
}

// ─── Initialize ─────────────────────────────────────────────────────────────

bool AudioCapture::initialize(const AudioEncoderConfig& config) {
    m_config = config;

    // Initialize COM for this thread
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_com_initialized = SUCCEEDED(hr);

    if (!init_device()) {
        LOG_ERROR("Failed to initialize audio device");
        return false;
    }

    if (!init_audio_client()) {
        LOG_ERROR("Failed to initialize audio client");
        return false;
    }

    LOG_INFO("Audio capture initialized: %d channels, %d Hz, %s",
             m_config.channels, m_config.sample_rate,
             audioCodecToString(m_config.codec).c_str());
    return true;
}

void AudioCapture::shutdown() {
    stop_capture();

    if (m_audio_clock) m_audio_clock->Release();
    if (m_capture_client) m_capture_client->Release();
    if (m_audio_client) m_audio_client->Release();
    if (m_audio_client3) m_audio_client3->Release();
    if (m_device) m_device->Release();
    if (m_device_enumerator) m_device_enumerator->Release();

    m_audio_clock = nullptr;
    m_capture_client = nullptr;
    m_audio_client = nullptr;
    m_audio_client3 = nullptr;
    m_device = nullptr;
    m_device_enumerator = nullptr;

    if (m_com_initialized) {
        CoUninitialize();
        m_com_initialized = false;
    }
}

bool AudioCapture::init_device() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&m_device_enumerator));
    if (FAILED(hr) || !m_device_enumerator) {
        LOG_ERROR("Failed to create MMDeviceEnumerator (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    // Get the default audio endpoint
    EDataFlow flow = eRender; // Default output
    if (m_config.source_type == AudioSourceType::Microphone) {
        flow = eCapture;
    } else {
        flow = eRender; // System audio loopback uses eRender
    }

    // For loopback recording (what you hear), we need eRender
    hr = m_device_enumerator->GetDefaultAudioEndpoint(flow, eConsole, &m_device);
    if (FAILED(hr) || !m_device) {
        LOG_ERROR("Failed to get default audio endpoint (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    LOG_DEBUG("Audio device initialized successfully");
    return true;
}

bool AudioCapture::init_audio_client() {
    // Activate audio client
    HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, 
                                    nullptr, reinterpret_cast<void**>(&m_audio_client));
    if (FAILED(hr) || !m_audio_client) {
        LOG_ERROR("Failed to activate audio client (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    // Get mix format
    WAVEFORMATEX* mix_format = nullptr;
    hr = m_audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        LOG_ERROR("Failed to get mix format (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    // Store format info
    m_config.channels = mix_format->nChannels;
    m_config.sample_rate = mix_format->nSamplesPerSec;

    // Safely check for float format (WAVEFORMATEXTENSIBLE only if format tag indicates it)
    bool format_is_ieee_float = false;
    if (mix_format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        format_is_ieee_float = true;
    } else if (mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && 
               mix_format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix_format);
        format_is_ieee_float = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    m_is_float = format_is_ieee_float;
    m_bytes_per_sample = mix_format->wBitsPerSample / 8;
    m_bytes_per_frame = m_config.channels * m_bytes_per_sample;

    LOG_INFO("Audio mix format: %d channels, %d Hz, %d bits, %s",
             m_config.channels, m_config.sample_rate, mix_format->wBitsPerSample,
             m_is_float ? "float" : "int");

    // Initialize audio client in loopback mode
    REFERENCE_TIME buffer_duration = 100000; // 10ms buffer

    // Try to use low-latency mode (Windows 10+)
    hr = m_audio_client->QueryInterface(__uuidof(IAudioClient3),
                                        reinterpret_cast<void**>(&m_audio_client3));
    if (SUCCEEDED(hr) && m_audio_client3) {
        UINT32 default_period = 0;
        UINT32 fundamental_period = 0;
        UINT32 min_period = 0;
        UINT32 max_period = 0;

        hr = m_audio_client3->GetSharedModeEnginePeriod(
            mix_format, &default_period, &fundamental_period, &min_period, &max_period);

        if (SUCCEEDED(hr)) {
            hr = m_audio_client3->InitializeSharedAudioStream(
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                fundamental_period * 2,
                mix_format,
                nullptr);
        } else {
            m_audio_client3->Release();
            m_audio_client3 = nullptr;
        }
    }

    if (!m_audio_client3) {
        // Fall back to standard initialization
        hr = m_audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            buffer_duration,
            0,
            mix_format,
            nullptr);
    }

    CoTaskMemFree(mix_format);

    if (FAILED(hr)) {
        LOG_ERROR("Failed to initialize audio client (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    // Get buffer size
    hr = m_audio_client->GetBufferSize(&m_buffer_frames);
    if (FAILED(hr)) {
        LOG_ERROR("Failed to get buffer size (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    // Get capture client
    hr = m_audio_client->GetService(__uuidof(IAudioCaptureClient),
                                    reinterpret_cast<void**>(&m_capture_client));
    if (FAILED(hr) || !m_capture_client) {
        LOG_ERROR("Failed to get capture client (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        return false;
    }

    // Get audio clock
    hr = m_audio_client->GetService(__uuidof(IAudioClock),
                                    reinterpret_cast<void**>(&m_audio_clock));

    return true;
}

// ─── Capture Control ────────────────────────────────────────────────────────

bool AudioCapture::start_capture(AudioFrameCallback on_audio) {
    if (m_capturing.load()) {
        LOG_WARN("Audio capture already running");
        return false;
    }

    m_on_audio = std::move(on_audio);
    m_should_stop = false;
    m_capturing = true;
    m_frames_captured = 0;

    // Start the audio client
    HRESULT hr = m_audio_client->Start();
    if (FAILED(hr)) {
        LOG_ERROR("Failed to start audio client (HRESULT: 0x%08X)", 
                  static_cast<unsigned int>(hr));
        m_capturing = false;
        return false;
    }

    m_capture_thread = std::thread(&AudioCapture::capture_thread_func, this);

    LOG_INFO("Audio capture started");
    return true;
}

void AudioCapture::stop_capture() {
    if (!m_capturing.load()) return;

    m_should_stop = true;
    m_capturing = false;

    if (m_capture_thread.joinable()) {
        m_capture_thread.join();
    }

    if (m_audio_client) {
        m_audio_client->Stop();
    }

    LOG_INFO("Audio capture stopped - captured %llu frames",
             static_cast<unsigned long long>(m_frames_captured.load()));
}

// ─── Capture Thread ─────────────────────────────────────────────────────────

void AudioCapture::capture_thread_func() {
    // Set thread priority for audio
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // MMCSS for glitch-free audio
    DWORD task_index = 0;
    HANDLE mmcss_handle = AvSetMmThreadCharacteristicsA("Audio", &task_index);

    while (!m_should_stop.load()) {
        UINT32 packet_size = 0;
        HRESULT hr = m_capture_client->GetNextPacketSize(&packet_size);

        if (FAILED(hr)) {
            LOG_ERROR("Audio capture client error (HRESULT: 0x%08X)", 
                      static_cast<unsigned int>(hr));
            break;
        }

        while (packet_size > 0) {
            BYTE* data = nullptr;
            UINT32 frames_available = 0;
            DWORD flags = 0;

            hr = m_capture_client->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (SUCCEEDED(hr) && data && frames_available > 0) {
                if (m_on_audio) {
                    AudioFrame frame;
                    frame.data = data;
                    frame.samples = frames_available;
                    frame.channels = m_config.channels;
                    frame.sample_rate = m_config.sample_rate;
                    
                    auto now = std::chrono::steady_clock::now();
                    frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch()).count();

                    m_on_audio(frame);
                    m_frames_captured += frames_available;
                }

                m_capture_client->ReleaseBuffer(frames_available);
            } else {
                // Release empty buffer to avoid hang
                if (frames_available > 0) {
                    m_capture_client->ReleaseBuffer(frames_available);
                }
                break;
            }

            hr = m_capture_client->GetNextPacketSize(&packet_size);
            if (FAILED(hr)) break;
        }

        // Sleep a bit to avoid busy waiting
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (mmcss_handle) {
        AvRevertMmThreadCharacteristics(mmcss_handle);
    }
}
