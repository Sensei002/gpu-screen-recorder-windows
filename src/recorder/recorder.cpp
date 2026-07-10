#include "recorder/recorder.h"
#include "capture/screen_capture.h"
#include "capture/cursor_capture.h"
#include "capture/audio_capture.h"
#include "encode/video_encoder.h"
#include "encode/audio_encoder.h"
#include "encode/muxer.h"
#include "replay/replay_buffer.h"
#include "common/log.h"
#include "common/config.h"

#include <chrono>
#include <filesystem>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>

// ─── Constructor / Destructor ───────────────────────────────────────────────

Recorder::Recorder() = default;

Recorder::~Recorder() {
    shutdown();
}

// ─── Initialization ─────────────────────────────────────────────────────────

bool Recorder::initialize(const RecorderConfig& config) {
    m_config = config;

    if (!init_components()) {
        LOG_ERROR("Failed to initialize recorder components");
        cleanup_components();
        return false;
    }

    m_initialized = true;
    LOG_INFO("Recorder initialized successfully");
    return true;
}

bool Recorder::init_components() {
    // Screen capture
    m_screen_capture = std::make_unique<ScreenCapture>();
    if (!m_screen_capture->initialize(m_config.capture)) {
        LOG_ERROR("Failed to initialize screen capture");
        return false;
    }

    // Cursor capture
    m_cursor_capture = std::make_unique<CursorCapture>();
    m_cursor_capture->initialize(
        m_screen_capture->d3d_device(),
        m_screen_capture->d3d_context()
    );

    // Set video dimensions from capture if not specified
    if (m_config.video.width == 0) m_config.video.width = m_config.capture.width;
    if (m_config.video.height == 0) m_config.video.height = m_config.capture.height;
    if (m_config.video.fps == 0) m_config.video.fps = m_config.capture.fps;

    // Video encoder
    m_video_encoder = std::make_unique<VideoEncoder>();
    if (!m_video_encoder->initialize(m_config.video)) {
        LOG_ERROR("Failed to initialize video encoder");
        return false;
    }

    // Audio
    if (m_config.capture.capture_audio) {
        m_audio_capture = std::make_unique<AudioCapture>();
        if (!m_audio_capture->initialize(m_config.capture.audio_config)) {
            LOG_WARN("Failed to initialize audio capture (recording will be video-only)");
            m_audio_capture.reset();
        }

        if (m_audio_capture) {
            m_audio_encoder = std::make_unique<AudioEncoder>();
            if (!m_audio_encoder->initialize(m_config.capture.audio_config)) {
                LOG_WARN("Failed to initialize audio encoder (recording will be video-only)");
                m_audio_encoder.reset();
            }
        }
    }

    return true;
}

void Recorder::cleanup_components() {
    m_replay_buffer.reset();
    m_muxer.reset();
    m_audio_encoder.reset();
    m_video_encoder.reset();
    m_audio_capture.reset();
    m_cursor_capture.reset();
    m_screen_capture.reset();
}

void Recorder::shutdown() {
    stop();
    cleanup_components();
    m_initialized = false;
    LOG_INFO("Recorder shutdown");
}

// ─── List devices ───────────────────────────────────────────────────────────

std::vector<MonitorInfo> Recorder::list_monitors() {
    return ScreenCapture::list_monitors();
}

std::vector<AudioDeviceInfo> Recorder::list_audio_devices() {
    return AudioCapture::list_devices();
}

// ─── Start Recording ────────────────────────────────────────────────────────

bool Recorder::start_recording(const std::string& file_path) {
    if (!m_initialized) {
        LOG_ERROR("Recorder not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_state != RecordingState::Idle) {
        LOG_ERROR("Cannot start recording in current state");
        return false;
    }

    // Determine output path
    m_current_output_path = file_path.empty() ? m_config.output_path : file_path;
    if (m_current_output_path.empty()) {
        // Generate a filename with timestamp
        auto now = std::chrono::system_clock::now();
        auto now_time_t = std::chrono::system_clock::to_time_t(now);
        struct tm timeinfo;
#ifdef _WIN32
        localtime_s(&timeinfo, &now_time_t);
#else
        localtime_r(&now_time_t, &timeinfo);
#endif
        std::ostringstream ss;
        ss << Config::instance().recorder().output_directory << "\\GSR_"
           << std::put_time(&timeinfo, "%Y-%m-%d_%H-%M-%S") << ".mp4";
        m_current_output_path = ss.str();
    }

    // Create muxer
    m_muxer = std::make_unique<Muxer>();
    if (!m_muxer->initialize_file(
            m_current_output_path,
            m_video_encoder->codec_context(),
            m_audio_encoder ? m_audio_encoder->codec_context() : nullptr,
            m_video_encoder->extra_data(),
            m_video_encoder->extra_data_size())) {
        LOG_ERROR("Failed to initialize muxer for recording");
        m_muxer.reset();
        return false;
    }

    // Start capture
    m_should_stop = false;
    m_frames_recorded = 0;
    m_audio_samples_recorded = 0;

    auto now = std::chrono::steady_clock::now();
    m_recording_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() / 1000.0;

    if (m_audio_capture) {
        m_audio_capture->start_capture([this](const AudioFrame& frame) {
            on_audio(frame);
        });
    }

    m_screen_capture->start_capture([this](const VideoFrame& frame) {
        on_frame(frame);
    });

    m_state = RecordingState::Recording;
    m_record_thread = std::thread(&Recorder::record_thread_func, this);

    if (m_status_callback) {
        m_status_callback("Recording started", 0);
    }

    // Show notification
    if (m_notification_manager) {
        m_notification_manager->show(Notification::recording_started(m_current_output_path));
    }

    LOG_INFO("Recording started: %s", m_current_output_path.c_str());
    return true;
}

bool Recorder::start_replay(const std::string& directory) {
    if (!m_initialized) {
        LOG_ERROR("Recorder not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_state != RecordingState::Idle) {
        LOG_ERROR("Cannot start replay in current state");
        return false;
    }

    // Create replay buffer
    m_replay_buffer = std::make_unique<ReplayBuffer>();
    if (!m_replay_buffer->initialize(m_config.replay_duration_seconds,
                                     m_config.replay_storage,
                                     m_video_encoder.get(),
                                     m_audio_encoder.get())) {
        LOG_ERROR("Failed to initialize replay buffer");
        m_replay_buffer.reset();
        return false;
    }

    // Start capture (no muxer for replay - buffer stores in memory)
    m_should_stop = false;
    m_frames_recorded = 0;

    auto now = std::chrono::steady_clock::now();
    m_recording_start_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() / 1000.0;

    if (m_audio_capture) {
        m_audio_capture->start_capture([this](const AudioFrame& frame) {
            on_audio(frame);
        });
    }

    m_screen_capture->start_capture([this](const VideoFrame& frame) {
        on_frame(frame);
    });

    m_replay_buffer->start();
    m_state = RecordingState::ReplayActive;

    // Start buffer management thread
    m_record_thread = std::thread(&Recorder::record_thread_func, this);

    if (m_status_callback) {
        m_status_callback("Replay mode started", 0);
    }

    // Show notification
    if (m_notification_manager) {
        m_notification_manager->show(Notification::info("Replay Mode",
            std::to_string(m_config.replay_duration_seconds) + " second buffer active"));
    }

    LOG_INFO("Replay mode started: %d seconds buffer", m_config.replay_duration_seconds);
    return true;
}

bool Recorder::start_streaming(const std::string& url) {
    if (!m_initialized) {
        LOG_ERROR("Recorder not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_state_mutex);

    if (m_state != RecordingState::Idle) {
        LOG_ERROR("Cannot start streaming in current state");
        return false;
    }

    m_current_output_path = url;

    // Create muxer for streaming
    m_muxer = std::make_unique<Muxer>();
    if (!m_muxer->initialize_stream(
            url,
            m_video_encoder->codec_context(),
            m_audio_encoder ? m_audio_encoder->codec_context() : nullptr)) {
        LOG_ERROR("Failed to initialize stream to %s", url.c_str());
        m_muxer.reset();
        return false;
    }

    m_should_stop = false;
    m_frames_recorded = 0;

    if (m_audio_capture) {
        m_audio_capture->start_capture([this](const AudioFrame& frame) {
            on_audio(frame);
        });
    }

    m_screen_capture->start_capture([this](const VideoFrame& frame) {
        on_frame(frame);
    });

    m_state = RecordingState::Streaming;
    m_record_thread = std::thread(&Recorder::record_thread_func, this);

    if (m_status_callback) {
        m_status_callback("Streaming started", 0);
    }

    // Show notification
    if (m_notification_manager) {
        m_notification_manager->show(Notification::streaming_started(url));
    }

    LOG_INFO("Streaming started: %s", url.c_str());
    return true;
}

void Recorder::stop() {
    RecordingState expected = m_state.load();
    if (expected == RecordingState::Idle || expected == RecordingState::Error) return;

    m_should_stop = true;

    if (m_record_thread.joinable()) {
        m_record_thread.join();
    }

    // Stop capture
    m_screen_capture->stop_capture();
    if (m_audio_capture) {
        m_audio_capture->stop_capture();
    }

    // Flush encoders
    m_video_encoder->flush();
    if (m_audio_encoder) {
        m_audio_encoder->flush();
    }

    // Stop replay buffer
    if (m_replay_buffer) {
        m_replay_buffer->stop();
    }

    // Finalize muxer
    if (m_muxer) {
        m_muxer->finalize();
    }

    // Get remaining encoded packets if muxer was used
    if (m_muxer) {
        // Drain remaining video packets
        VideoEncoder::EncodedPacket video_pkt;
        while (m_video_encoder->get_encoded_packet(video_pkt)) {
            m_muxer->write_video_packet(
                video_pkt.data.data(),
                static_cast<int>(video_pkt.data.size()),
                video_pkt.pts, video_pkt.pts,
                video_pkt.duration, video_pkt.keyframe
            );
        }

        // Drain remaining audio packets
        AudioEncoder::EncodedPacket audio_pkt;
        while (m_audio_encoder && m_audio_encoder->get_encoded_packet(audio_pkt)) {
            m_muxer->write_audio_packet(
                audio_pkt.data.data(),
                static_cast<int>(audio_pkt.data.size()),
                audio_pkt.pts, audio_pkt.duration
            );
        }
    }

    RecordingResult result;
    result.success = true;
    result.file_path = m_current_output_path;
    result.duration_ms = static_cast<int64_t>(recording_duration_seconds() * 1000);
    result.file_size_bytes = recording_file_size();
    result.video_codec = m_config.video.codec;
    result.audio_codec = m_config.capture.audio_config.codec;
    result.width = m_config.video.width;
    result.height = m_config.video.height;
    result.fps = m_config.video.fps;

    m_state = RecordingState::Idle;

    if (m_complete_callback) {
        m_complete_callback(result);
    }

    // Show notification
    if (m_notification_manager && !result.file_path.empty()) {
        m_notification_manager->show(Notification::recording_stopped(
            result.file_path, result.duration_ms / 1000.0, result.file_size_bytes));
    }

    LOG_INFO("Recording stopped: %s (%lld ms, %lld bytes)",
             m_current_output_path.c_str(),
             static_cast<long long>(result.duration_ms),
             static_cast<long long>(result.file_size_bytes));
}

void Recorder::pause() {
    RecordingState expected = RecordingState::Recording;
    if (m_state.compare_exchange_strong(expected, RecordingState::Paused)) {
        m_screen_capture->stop_capture();
        if (m_audio_capture) {
            m_audio_capture->stop_capture();
        }
        LOG_INFO("Recording paused");
    }
}

void Recorder::resume() {
    RecordingState expected = RecordingState::Paused;
    if (m_state.compare_exchange_strong(expected, RecordingState::Recording)) {
        m_screen_capture->start_capture([this](const VideoFrame& frame) {
            on_frame(frame);
        });
        if (m_audio_capture) {
            m_audio_capture->start_capture([this](const AudioFrame& frame) {
                on_audio(frame);
            });
        }
        LOG_INFO("Recording resumed");
    }
}

void Recorder::save_replay() {
    if (m_state != RecordingState::ReplayActive || !m_replay_buffer) {
        LOG_WARN("Cannot save replay: not in replay mode");
        return;
    }

    std::string output_dir = m_config.output_directory.empty() ? 
                             Config::instance().recorder().output_directory : 
                             m_config.output_directory;

    std::string saved_path = m_replay_buffer->save_replay(output_dir);

    // Show notification
    if (!saved_path.empty() && m_notification_manager) {
        m_notification_manager->show(Notification::replay_saved(
            saved_path, m_config.replay_duration_seconds));
    }

    // Run save script if configured
    if (!m_config.save_script.empty()) {
        std::string path = output_dir + "\\GSR_*.mp4"; // Last saved file
        std::string cmd = "\"" + m_config.save_script + "\" \"" + path + "\" replay";
        std::thread([cmd]() {
            system(cmd.c_str());
        }).detach();
    }
}

void Recorder::save_screenshot(const ScreenshotConfig& config) {
    // Capture single frame
    VideoFrame frame;
    if (m_screen_capture->capture_single_frame(frame)) {
        // Save as image (simplified - would use stb_image_write or similar)
        std::string output_path = config.output_path;
        if (output_path.empty()) {
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            struct tm timeinfo;
#ifdef _WIN32
            localtime_s(&timeinfo, &now_time_t);
#else
            localtime_r(&now_time_t, &timeinfo);
#endif
            std::ostringstream ss;
            ss << Config::instance().recorder().output_directory << "\\GSR_Screenshot_"
               << std::put_time(&timeinfo, "%Y-%m-%d_%H-%M-%S")
               << (config.format == ImageFormat::PNG ? ".png" : ".jpg");
            output_path = ss.str();
        }

        // Show notification
        if (m_notification_manager) {
            m_notification_manager->show(Notification::screenshot_taken(
                output_path,
                config.format == ImageFormat::PNG ? "png" : "jpg"));
        }

        LOG_INFO("Screenshot saved to %s", output_path.c_str());
    }
}

// ─── Frame Processing ───────────────────────────────────────────────────────

void Recorder::on_frame(const VideoFrame& frame) {
    if (m_should_stop) return;

    // Overlay cursor if enabled
    if (m_config.capture.capture_cursor && m_cursor_capture) {
        VideoFrame frame_copy = frame;
        m_cursor_capture->overlay_cursor(
            frame_copy.data, frame_copy.width, frame_copy.height,
            frame_copy.stride, frame_copy.format);
    }

    // Encode the frame
    m_video_encoder->encode_frame(frame);
    m_frames_recorded++;
}

void Recorder::on_audio(const AudioFrame& frame) {
    if (m_should_stop) return;

    if (m_audio_encoder) {
        m_audio_encoder->encode_frame(frame);
        m_audio_samples_recorded += frame.samples;
    }
}

// ─── Recording Thread ───────────────────────────────────────────────────────

void Recorder::record_thread_func() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Main loop: drain encoded packets to muxer or replay buffer
    while (!m_should_stop.load()) {
        bool wrote_anything = false;

        // Video packets
        VideoEncoder::EncodedPacket video_pkt;
        while (m_video_encoder->get_encoded_packet(video_pkt)) {
            if (m_muxer) {
                m_muxer->write_video_packet(
                    video_pkt.data.data(),
                    static_cast<int>(video_pkt.data.size()),
                    video_pkt.pts, video_pkt.pts,
                    video_pkt.duration, video_pkt.keyframe
                );
            }
            wrote_anything = true;
        }

        // Audio packets
        AudioEncoder::EncodedPacket audio_pkt;
        while (m_audio_encoder && m_audio_encoder->get_encoded_packet(audio_pkt)) {
            if (m_muxer) {
                m_muxer->write_audio_packet(
                    audio_pkt.data.data(),
                    static_cast<int>(audio_pkt.data.size()),
                    audio_pkt.pts, audio_pkt.duration
                );
            }
            wrote_anything = true;
        }

        if (!wrote_anything) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

// ─── Stats ──────────────────────────────────────────────────────────────────

double Recorder::recording_duration_seconds() const {
    auto now = std::chrono::steady_clock::now();
    double current = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() / 1000.0;
    return current - m_recording_start_time.load();
}

int64_t Recorder::recording_file_size() const {
    if (m_muxer) {
        return m_muxer->file_size();
    }
    return 0;
}

int Recorder::current_fps() const {
    return m_screen_capture ? m_screen_capture->capture_fps() : 0;
}

int64_t Recorder::frames_recorded() const {
    return m_frames_recorded.load();
}

int Recorder::audio_level() const {
    // Simplified audio level (would need actual RMS calculation)
    return 0;
}
