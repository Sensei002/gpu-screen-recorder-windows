#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "common/types.h"
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include <string>

struct IAudioClient;
struct IAudioCaptureClient;
struct IAudioClient3;
struct IMMDevice;
struct IMMDeviceEnumerator;
struct IAudioClock;

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool initialize(const AudioEncoderConfig& config);
    void shutdown();

    // List available audio devices
    static std::vector<AudioDeviceInfo> list_devices();
    static std::vector<std::string> list_application_audio();

    // Start/stop capture
    bool start_capture(AudioFrameCallback on_audio);
    void stop_capture();

    bool is_capturing() const { return m_capturing.load(); }

private:
    bool init_device();
    bool init_audio_client();
    void capture_thread_func();

    // COM initialization state
    bool m_com_initialized = false;

    // Audio device
    IMMDeviceEnumerator* m_device_enumerator = nullptr;
    IMMDevice* m_device = nullptr;
    IAudioClient* m_audio_client = nullptr;
    IAudioClient3* m_audio_client3 = nullptr; // For low-latency mode
    IAudioCaptureClient* m_capture_client = nullptr;
    IAudioClock* m_audio_clock = nullptr;

    // Config
    AudioEncoderConfig m_config;

    // Format info
    int m_bytes_per_frame = 0;
    UINT32 m_buffer_frames = 0;
    bool m_is_float = false;
    int m_bytes_per_sample = 0;

    // Threading
    std::thread m_capture_thread;
    std::atomic<bool> m_capturing{false};
    std::atomic<bool> m_should_stop{false};
    std::atomic<uint64_t> m_frames_captured{0};

    // Callback
    AudioFrameCallback m_on_audio;
};
