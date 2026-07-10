#include "replay/replay_buffer.h"
#include "common/log.h"
#include "encode/video_encoder.h"
#include "encode/audio_encoder.h"
#include "encode/muxer.h"

#include <chrono>
#include <filesystem>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>

ReplayBuffer::ReplayBuffer() = default;

ReplayBuffer::~ReplayBuffer() {
    shutdown();
}

bool ReplayBuffer::initialize(int duration_seconds, ReplayStorage storage,
                              VideoEncoder* video_encoder, AudioEncoder* audio_encoder) {
    m_duration_seconds = duration_seconds;
    m_storage = storage;
    m_video_encoder = video_encoder;
    m_audio_encoder = audio_encoder;

    if (!m_video_encoder || !m_video_encoder->is_initialized()) {
        LOG_ERROR("Replay buffer requires an initialized video encoder");
        return false;
    }

    LOG_INFO("Replay buffer initialized: %d seconds, storage: %s",
             duration_seconds, storage == ReplayStorage::RAM ? "RAM" : "Disk");
    return true;
}

void ReplayBuffer::shutdown() {
    stop();

    std::lock_guard<std::mutex> lock(m_buffer_mutex);
    m_segments.clear();

    LOG_INFO("Replay buffer shutdown");
}

bool ReplayBuffer::start() {
    if (m_active.load()) return true;

    m_should_stop = false;
    m_active = true;

    auto now = std::chrono::steady_clock::now();
    m_buffer_start_time = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // Start the buffer management thread to poll encoders
    m_manage_thread = std::thread(&ReplayBuffer::manage_buffer, this);

    LOG_INFO("Replay buffer started");
    return true;
}

void ReplayBuffer::stop() {
    if (!m_active.load()) return;

    m_should_stop = true;
    m_active = false;

    if (m_manage_thread.joinable()) {
        m_manage_thread.join();
    }

    LOG_INFO("Replay buffer stopped");
}

int ReplayBuffer::buffered_seconds() const {
    std::lock_guard<std::mutex> lock(const_cast<ReplayBuffer*>(this)->m_buffer_mutex);
    if (m_segments.empty()) return 0;

    auto now = std::chrono::steady_clock::now();
    int64_t current_time = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    int64_t oldest_time = m_segments.front().timestamp_us;
    int64_t newest_time = m_segments.back().timestamp_us;

    return static_cast<int>((newest_time - oldest_time) / 1000000);
}

// ─── Buffer management ─────────────────────────────────────────────────────

void ReplayBuffer::manage_buffer() {
    // Poll encoder for new packets and store them
    while (!m_should_stop.load()) {
        {
            std::lock_guard<std::mutex> lock(m_buffer_mutex);

            // Get video packets
            VideoEncoder::EncodedPacket video_pkt;
            while (m_video_encoder->get_encoded_packet(video_pkt)) {
                ReplaySegment segment;
                segment.video_data = std::move(video_pkt.data);
                segment.video_pts = video_pkt.pts;
                segment.video_duration = video_pkt.duration;
                segment.video_keyframe = video_pkt.keyframe;

                auto now = std::chrono::steady_clock::now();
                segment.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now.time_since_epoch()).count();

                m_segments.push_back(std::move(segment));
                m_segment_count++;
                m_buffer_size_bytes += static_cast<int>(segment.video_data.size());

                // Remove oldest segments to maintain buffer duration
                while (!m_segments.empty() && 
                       (m_segments.back().timestamp_us - m_segments.front().timestamp_us) > 
                       static_cast<int64_t>(m_duration_seconds) * 1000000) {
                    m_buffer_size_bytes -= static_cast<int>(m_segments.front().video_data.size());
                    m_segments.pop_front();
                }
            }

            // Get audio packets
            AudioEncoder::EncodedPacket audio_pkt;
            while (m_audio_encoder->get_encoded_packet(audio_pkt)) {
                // Attach to latest video segment if exists
                if (!m_segments.empty()) {
                    m_segments.back().audio_data = std::move(audio_pkt.data);
                    m_segments.back().audio_pts = audio_pkt.pts;
                    m_segments.back().audio_duration = audio_pkt.duration;
                    m_buffer_size_bytes += static_cast<int>(m_segments.back().audio_data.size());
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ─── Save replay ────────────────────────────────────────────────────────────

std::string ReplayBuffer::save_replay(const std::string& output_dir) {
    std::lock_guard<std::mutex> lock(m_buffer_mutex);

    if (m_segments.empty()) {
        LOG_WARN("No replay data to save");
        return "";
    }

    std::string filename = generate_filename(output_dir);
    LOG_INFO("Saving replay to %s...", filename.c_str());

    // Create muxer for saving
    Muxer muxer;
    bool initialized = muxer.initialize_file(
        filename,
        m_video_encoder->codec_context(),
        m_audio_encoder ? m_audio_encoder->codec_context() : nullptr,
        m_video_encoder->extra_data(),
        m_video_encoder->extra_data_size()
    );

    if (!initialized) {
        LOG_ERROR("Failed to initialize muxer for replay save");
        return "";
    }

    // Write all segments
    int segments_written = 0;
    for (const auto& segment : m_segments) {
        bool wrote = false;

        if (!segment.video_data.empty()) {
            wrote = muxer.write_video_packet(
                segment.video_data.data(),
                static_cast<int>(segment.video_data.size()),
                segment.video_pts,
                segment.video_pts,
                segment.video_duration,
                segment.video_keyframe
            );
        }

        if (!segment.audio_data.empty()) {
            muxer.write_audio_packet(
                segment.audio_data.data(),
                static_cast<int>(segment.audio_data.size()),
                segment.audio_pts,
                segment.audio_duration
            );
        }

        if (wrote) segments_written++;
    }

    muxer.finalize();

    LOG_INFO("Replay saved: %s (%d segments, %d bytes)",
             filename.c_str(), segments_written, m_buffer_size_bytes.load());

    return filename;
}

bool ReplayBuffer::save_replay_async(const std::string& output_dir,
                                     std::function<void(bool, const std::string&)> on_complete) {
    // Simple async via thread
    std::thread([this, output_dir, on_complete]() {
        std::string path = save_replay(output_dir);
        if (on_complete) {
            on_complete(!path.empty(), path);
        }
    }).detach();

    return true;
}

std::string ReplayBuffer::generate_filename(const std::string& dir) {
    // Generate filename with timestamp: GPU_Screen_Recorder_YYYY-MM-DD_HH-MM-SS.mp4
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);

    struct tm timeinfo;
#ifdef _WIN32
    localtime_s(&timeinfo, &now_time_t);
#else
    localtime_r(&now_time_t, &timeinfo);
#endif

    std::ostringstream ss;
    ss << dir;
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') {
        ss << "\\";
    }
    ss << "GSR_"
       << std::put_time(&timeinfo, "%Y-%m-%d_%H-%M-%S")
       << ".mp4";

    // Ensure directory exists
    std::filesystem::path dir_path = std::filesystem::path(ss.str()).parent_path();
    std::filesystem::create_directories(dir_path);

    return ss.str();
}
