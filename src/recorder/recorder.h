#pragma once

#include "common/types.h"
#include "ui/notification.h"
#include <memory>
#include <atomic>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>

class ScreenCapture;
class CursorCapture;
class AudioCapture;
class VideoEncoder;
class AudioEncoder;
class Muxer;
class ReplayBuffer;

class Recorder {
public:
    Recorder();
    ~Recorder();

    // Prevent copy
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Initialize the recorder (must be called before start)
    bool initialize(const RecorderConfig& config);
    void shutdown();

    // Start recording / replay / streaming
    bool start_recording(const std::string& file_path = "");
    bool start_replay(const std::string& directory = "");
    bool start_streaming(const std::string& url);
    void stop();

    // Control signals
    void pause();
    void resume();
    void save_replay();
    void save_screenshot(const ScreenshotConfig& config = {});

    // Get state
    RecordingState state() const { return m_state.load(); }
    bool is_recording() const { return m_state == RecordingState::Recording; }
    bool is_replay_active() const { return m_state == RecordingState::ReplayActive; }
    bool is_streaming() const { return m_state == RecordingState::Streaming; }
    bool is_paused() const { return m_state == RecordingState::Paused; }

    // Get stats
    double recording_duration_seconds() const;
    int64_t recording_file_size() const;
    int current_fps() const;
    int64_t frames_recorded() const;
    int audio_level() const;

    // Callbacks
    using StatusCallback = std::function<void(const std::string& status, int progress)>;
    void set_status_callback(StatusCallback cb) { m_status_callback = std::move(cb); }

    using ErrorCallback = std::function<void(const std::string& error)>;
    void set_error_callback(ErrorCallback cb) { m_error_callback = std::move(cb); }

    using RecordingCompleteCallback = std::function<void(const RecordingResult&)>;
    void set_complete_callback(RecordingCompleteCallback cb) { m_complete_callback = std::move(cb); }

    // List capture options
    static std::vector<MonitorInfo> list_monitors();
    static std::vector<AudioDeviceInfo> list_audio_devices();

    // Get capture/encode devices
    ScreenCapture* screen_capture() const { return m_screen_capture.get(); }
    AudioCapture* audio_capture() const { return m_audio_capture.get(); }

private:
    // Internal initialization
    bool init_components();
    void cleanup_components();

    // Frame processing
    void on_frame(const VideoFrame& frame);
    void on_audio(const AudioFrame& frame);

    // Recording thread functions
    void record_thread_func();
    void replay_buffer_thread_func();

    // Components
    std::unique_ptr<ScreenCapture> m_screen_capture;
    std::unique_ptr<CursorCapture> m_cursor_capture;
    std::unique_ptr<AudioCapture> m_audio_capture;
    std::unique_ptr<VideoEncoder> m_video_encoder;
    std::unique_ptr<AudioEncoder> m_audio_encoder;
    std::unique_ptr<Muxer> m_muxer;
    std::unique_ptr<ReplayBuffer> m_replay_buffer;

    // Config
    RecorderConfig m_config;
    bool m_initialized = false;

    // Notifications
    std::shared_ptr<NotificationManager> m_notification_manager;

    // State
    std::atomic<RecordingState> m_state{RecordingState::Idle};
    std::atomic<bool> m_should_stop{false};
    std::mutex m_state_mutex;

    // Threads
    std::thread m_record_thread;

    // Statistics
    std::atomic<int64_t> m_frames_recorded{0};
    std::atomic<int64_t> m_audio_samples_recorded{0};
    std::atomic<double> m_recording_start_time{0};
    std::atomic<int> m_current_fps{0};

    // Callbacks
    StatusCallback m_status_callback;
    ErrorCallback m_error_callback;
    RecordingCompleteCallback m_complete_callback;

    // Output path
    std::string m_current_output_path;

public:
    // Set notification manager
    void set_notification_manager(std::shared_ptr<NotificationManager> mgr) { m_notification_manager = mgr; }
};
