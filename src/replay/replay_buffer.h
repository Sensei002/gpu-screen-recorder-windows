#pragma once

#include "common/types.h"
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <functional>

class VideoEncoder;
class AudioEncoder;
class Muxer;

class ReplayBuffer {
public:
    ReplayBuffer();
    ~ReplayBuffer();

    // Initialize the replay buffer
    bool initialize(int duration_seconds, ReplayStorage storage,
                    VideoEncoder* video_encoder, AudioEncoder* audio_encoder);
    void shutdown();

    // Start/stop replay mode
    bool start();
    void stop();

    // Save the current replay buffer to a file
    // Returns the file path
    std::string save_replay(const std::string& output_dir);
    bool save_replay_async(const std::string& output_dir,
                           std::function<void(bool, const std::string&)> on_complete);

    // Check if replay is active
    bool is_active() const { return m_active.load(); }

    // Get replay info
    int duration_seconds() const { return m_duration_seconds; }
    int buffered_seconds() const;

private:
    struct ReplaySegment {
        std::vector<uint8_t> video_data;
        std::vector<uint8_t> audio_data;
        int64_t video_pts = 0;
        int64_t audio_pts = 0;
        int64_t video_duration = 0;
        int64_t audio_duration = 0;
        bool video_keyframe = false;
        int64_t timestamp_us = 0;
    };

    void manage_buffer();
    std::string generate_filename(const std::string& dir);

    // Configuration
    int m_duration_seconds = 30;
    ReplayStorage m_storage = ReplayStorage::RAM;
    VideoEncoder* m_video_encoder = nullptr;
    AudioEncoder* m_audio_encoder = nullptr;

    // Buffer
    std::deque<ReplaySegment> m_segments;
    std::mutex m_buffer_mutex;
    std::atomic<bool> m_active{false};

    // Buffer management thread
    std::thread m_manage_thread;
    std::atomic<bool> m_should_stop{false};

    // Timestamp tracking
    int64_t m_buffer_start_time = 0;
    std::atomic<int64_t> m_last_frame_time{0};

    // Statistics
    std::atomic<int> m_segment_count{0};
    std::atomic<int> m_buffer_size_bytes{0};
};
