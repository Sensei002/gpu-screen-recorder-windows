// Include d3d11.h before overlay.h to ensure D3D11 COM types are defined
// overlay.h only forward-declares them, but we need the full definitions.
#include <d3d11.h>

#include "ui/ui_renderer.h"
#include "ui/overlay.h"
#include "common/config.h"
#include "common/log.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <mmsystem.h>
#include <cstdio>
#include <string>
#include <format>
#include <algorithm>

// ─── Constructor / Destructor ───────────────────────────────────────────────

UIRenderer::UIRenderer() = default;

UIRenderer::~UIRenderer() {
    shutdown();
}

// ─── Initialize ─────────────────────────────────────────────────────────────

bool UIRenderer::initialize(Overlay* overlay) {
    m_overlay = overlay;

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Don't save/load settings

    // Apply custom theme
    apply_theme();

    // Initialize ImGui backends
    if (!ImGui_ImplWin32_Init(overlay->hwnd())) {
        LOG_ERROR("Failed to initialize ImGui Win32 backend");
        return false;
    }

    if (!ImGui_ImplDX11_Init(overlay->d3d_device(), overlay->d3d_context())) {
        LOG_ERROR("Failed to initialize ImGui DX11 backend");
        return false;
    }

    LOG_INFO("UI renderer initialized: ImGui %s", ImGui::GetVersion());
    return true;
}

void UIRenderer::shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

// ─── Render Frame ───────────────────────────────────────────────────────────

void UIRenderer::render_frame() {
    auto* overlay = m_overlay;
    if (!overlay) return;

    auto* d3d_context = overlay->d3d_context();
    auto* swap_chain = overlay->swap_chain();
    auto* render_target = overlay->render_target();

    if (!d3d_context || !swap_chain || !render_target) return;

    // Start ImGui frame
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Set up viewport for overlay
    RECT rect;
    GetWindowRect(overlay->window(), &rect);
    int overlay_width = rect.right - rect.left;
    int overlay_height = rect.bottom - rect.top;

    // Render the appropriate page
    switch (m_current_page) {
        case Page::Main:
            render_main_window();
            break;
        case Page::Recording:
            render_recording_panel();
            break;
        case Page::Replay:
            render_replay_panel();
            break;
        case Page::Settings:
            render_settings_panel();
            break;
        case Page::About:
            render_about_tab();
            break;
    }

    // Always render stats overlay
    if (m_show_stats_overlay && (m_is_recording || m_is_replay_mode)) {
        render_stats_overlay();
    }

    // Render
    ImGui::Render();
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    d3d_context->OMSetRenderTargets(1, &render_target, nullptr);
    d3d_context->ClearRenderTargetView(render_target, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Present with transparency
    swap_chain->Present(1, 0); // VSync on
}

// ─── Main Window ────────────────────────────────────────────────────────────

void UIRenderer::render_main_window() {
    auto& data = m_overlay->overlay_data();
    auto state = m_overlay->recorder_state();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.85f);

    ImGui::Begin("GPU Screen Recorder", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar);

    // ─── Header ──────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.8f, 0.3f, 1.0f));
    ImGui::SetWindowFontScale(1.5f);
    ImGui::Text("GPU Screen Recorder");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::Separator();
    ImGui::Spacing();

    // ─── Status ──────────────────────────────────────────────────────────
    switch (state) {
        case RecordingState::Idle:
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Status: Idle");
            break;
        case RecordingState::Recording:
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "● RECORDING");
            break;
        case RecordingState::ReplayActive:
            ImGui::TextColored(ImVec4(0.2f, 0.6f, 1.0f, 1.0f), "● REPLAY ACTIVE");
            break;
        case RecordingState::Streaming:
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f), "● STREAMING");
            break;
        case RecordingState::Paused:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "⏸ PAUSED");
            break;
        default:
            break;
    }

    // Show stats
    ImGui::Spacing();
    if (data.replay_mode) {
        ImGui::Text("Replay Buffer: %d seconds", data.replay_duration);
    }
    ImGui::Text("FPS: %d", data.fps);
    if (data.frames_recorded > 0) {
        double minutes = data.recording_duration / 60.0;
        ImGui::Text("Duration: %.1f min (%.0f sec)", minutes, data.recording_duration);
        ImGui::Text("Frames: %lld", static_cast<long long>(data.frames_recorded));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ─── Recording controls ──────────────────────────────────────────────
    float button_width = ImGui::GetContentRegionAvail().x * 0.9f;

    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x * 0.05f);

    ImVec4 record_color = ImVec4(0.8f, 0.1f, 0.1f, 1.0f);
    ImVec4 replay_color = ImVec4(0.1f, 0.4f, 0.8f, 1.0f);
    ImVec4 settings_color = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);

    // Record Button
    if (state == RecordingState::Idle || state == RecordingState::ReplayActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, record_color);
        if (ImGui::Button("● Start Recording", ImVec2(button_width, 50))) {
            if (m_overlay->on_start_recording) m_overlay->on_start_recording();
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
        if (ImGui::Button("■ Stop Recording", ImVec2(button_width, 50))) {
            if (m_overlay->on_stop_recording) m_overlay->on_stop_recording();
        }
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x * 0.05f);

    // Save Replay Button
    if (state == RecordingState::ReplayActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, replay_color);
        if (ImGui::Button("Save Replay", ImVec2(button_width, 50))) {
            if (m_overlay->on_save_replay) m_overlay->on_save_replay();
        }
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x * 0.05f);

    // Settings Button
    ImGui::PushStyleColor(ImGuiCol_Button, settings_color);
    if (ImGui::Button("⚙ Settings", ImVec2(button_width, 40))) {
        m_current_page = Page::Settings;
        m_settings_tab = 0;
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x * 0.05f);

    // Screenshot Button
    if (ImGui::Button("📷 Screenshot", ImVec2(button_width, 40))) {
        if (m_overlay->on_take_screenshot) m_overlay->on_take_screenshot();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ─── Footer ──────────────────────────────────────────────────────────
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Alt+Z: Toggle UI | F8: Save Replay | F9: Record");
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "Encoder: %s", data.encoder_name.c_str());

    ImGui::End();
}

// ─── Recording Panel ────────────────────────────────────────────────────────

void UIRenderer::render_recording_panel() {
    ImGui::Begin("Recording", nullptr, ImGuiWindowFlags_NoCollapse);

    auto& data = m_overlay->overlay_data();
    ImGui::Text("Recording...");
    ImGui::Text("Duration: %.1f sec", data.recording_duration);
    ImGui::Text("Frames: %lld", static_cast<long long>(data.frames_recorded));
    ImGui::Text("FPS: %d", data.fps);

    if (ImGui::Button("Stop Recording")) {
        if (m_overlay->on_stop_recording) m_overlay->on_stop_recording();
        m_current_page = Page::Main;
    }

    ImGui::SameLine();
    if (ImGui::Button("Back")) {
        m_current_page = Page::Main;
    }

    ImGui::End();
}

// ─── Replay Panel ───────────────────────────────────────────────────────────

void UIRenderer::render_replay_panel() {
    ImGui::Begin("Replay Mode", nullptr, ImGuiWindowFlags_NoCollapse);

    auto& data = m_overlay->overlay_data();
    ImGui::Text("Instant replay is active");
    ImGui::Text("Buffer: %d seconds", data.replay_duration);
    ImGui::Text("Press F8 to save replay");

    ImGui::Spacing();
    if (ImGui::Button("Save Replay Now")) {
        if (m_overlay->on_save_replay) m_overlay->on_save_replay();
    }

    ImGui::SameLine();
    if (ImGui::Button("Stop Replay")) {
        if (m_overlay->on_stop_recording) m_overlay->on_stop_recording();
        m_current_page = Page::Main;
    }

    ImGui::End();
}

// ─── Settings Panel ─────────────────────────────────────────────────────────

void UIRenderer::render_settings_panel() {
    const char* tabs[] = {"Output", "Video", "Audio", "Hotkeys", "About"};
    int tab_count = IM_ARRAYSIZE(tabs);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);

    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove);

    // Header
    ImGui::TextColored(ImVec4(0.1f, 0.8f, 0.3f, 1.0f), "Settings");
    ImGui::Separator();

    // Tab bar
    for (int i = 0; i < tab_count; i++) {
        if (i > 0) ImGui::SameLine();
        if (ImGui::Button(tabs[i], ImVec2(120, 30))) {
            m_settings_tab = i;
        }
    }

    ImGui::Separator();
    ImGui::Spacing();

    switch (m_settings_tab) {
        case 0: render_output_tab(); break;
        case 1: render_video_tab(); break;
        case 2: render_audio_tab(); break;
        case 3: render_hotkeys_tab(); break;
        case 4: render_about_tab(); break;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("← Back", ImVec2(120, 30))) {
        m_current_page = Page::Main;
    }

    ImGui::End();
}

void UIRenderer::render_output_tab() {
    auto& recorder = Config::instance().recorder();

    ImGui::Text("Output Settings");
    ImGui::Spacing();

    // Output directory
    ImGui::Text("Output Directory:");
    static char output_dir[512] = {};
    strncpy_s(output_dir, recorder.output_directory.c_str(), sizeof(output_dir) - 1);
    ImGui::InputText("##output_dir", output_dir, sizeof(output_dir));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        recorder.output_directory = output_dir;
    }

    // Default codec
    ImGui::Spacing();
    ImGui::Text("Output Format:");
    const char* formats[] = {"MP4", "MKV", "TS", "FLV"};
    static int format_idx = 0;
    ImGui::Combo("##format", &format_idx, formats, IM_ARRAYSIZE(formats));

    // Replay settings
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Replay Mode");

    int replay_dur = recorder.replay_duration_seconds;
    ImGui::SliderInt("Duration (sec)", &replay_dur, 10, 300);
    recorder.replay_duration_seconds = replay_dur;

    const char* storage_types[] = {"RAM (faster)", "Disk (more capacity)"};
    int storage_idx = (recorder.replay_storage == ReplayStorage::Disk) ? 1 : 0;
    ImGui::Combo("Storage", &storage_idx, storage_types, IM_ARRAYSIZE(storage_types));
    recorder.replay_storage = (storage_idx == 1) ? ReplayStorage::Disk : ReplayStorage::RAM;

    // Save config
    ImGui::Spacing();
    if (ImGui::Button("Save Config")) {
        Config::instance().save();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to Defaults")) {
        Config::instance().load(); // Reload defaults
    }
}

void UIRenderer::render_video_tab() {
    auto& video = Config::instance().recorder().video;

    ImGui::Text("Video Settings");
    ImGui::Spacing();

    // Codec selection
    const char* codecs[] = {"H.264 (NVENC/AMF/QSV)", "HEVC/H.265", "AV1", "VP8", "VP9"};
    int codec_idx = static_cast<int>(video.codec);
    ImGui::Combo("Codec", &codec_idx, codecs, IM_ARRAYSIZE(codecs));
    video.codec = static_cast<VideoCodec>(codec_idx);

    // Quality
    ImGui::Spacing();
    ImGui::Text("Quality Mode:");
    bool is_cq = video.constant_quality;
    ImGui::Checkbox("Constant Quality", &is_cq);
    video.constant_quality = is_cq;

    if (video.constant_quality) {
        int quality = video.quality;
        ImGui::SliderInt("Quality (lower=better)", &quality, 0, 51);
        video.quality = quality;
    } else {
        int bitrate = video.bitrate_kbps;
        ImGui::SliderInt("Bitrate (kbps)", &bitrate, 1000, 200000);
        video.bitrate_kbps = bitrate;
    }

    // FPS
    int fps = video.fps;
    ImGui::SliderInt("FPS", &fps, 24, 240);
    video.fps = fps;

    // Keyframe interval
    int keyint = video.keyint_seconds;
    ImGui::SliderInt("Keyframe Interval (sec)", &keyint, 1, 10);
    video.keyint_seconds = keyint;

    // HDR
    ImGui::Checkbox("HDR Recording", &video.enable_hdr);
}

void UIRenderer::render_audio_tab() {
    auto& audio = Config::instance().recorder().capture.audio_config;

    ImGui::Text("Audio Settings");
    ImGui::Spacing();

    // Audio source
    const char* sources[] = {"System Audio (what you hear)", "Microphone", "Both"};
    int source_idx = static_cast<int>(audio.source_type);
    ImGui::Combo("Source", &source_idx, sources, IM_ARRAYSIZE(sources));
    audio.source_type = static_cast<AudioSourceType>(source_idx);

    // Codec
    const char* codecs[] = {"AAC", "Opus"};
    int codec_idx = (audio.codec == AudioCodec::Opus) ? 1 : 0;
    ImGui::Combo("Codec", &codec_idx, codecs, IM_ARRAYSIZE(codecs));
    audio.codec = (codec_idx == 1) ? AudioCodec::Opus : AudioCodec::AAC;

    // Bitrate
    int bitrate = audio.bitrate_kbps;
    ImGui::SliderInt("Bitrate (kbps)", &bitrate, 64, 512);
    audio.bitrate_kbps = bitrate;

    // Sample rate
    const char* rates[] = {"44100 Hz", "48000 Hz"};
    int rate_idx = (audio.sample_rate == 48000) ? 1 : 0;
    ImGui::Combo("Sample Rate", &rate_idx, rates, IM_ARRAYSIZE(rates));
    audio.sample_rate = (rate_idx == 1) ? 48000 : 44100;
}

void UIRenderer::render_hotkeys_tab() {
    auto& hotkeys = Config::instance().hotkeys();

    ImGui::Text("Hotkey Settings");
    ImGui::Spacing();

    ImGui::Checkbox("Enable Hotkeys", &hotkeys.enable_hotkeys);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Default Hotkeys:");
    ImGui::BulletText("Alt+Z: Toggle UI overlay");
    ImGui::BulletText("F8: Save replay");
    ImGui::BulletText("F9: Start/Stop recording");
    ImGui::BulletText("Ctrl+Shift+Alt+Esc: Emergency exit");

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f),
              "Hotkey customization coming in a future update.");
}

void UIRenderer::render_about_tab() {
    auto& hotkeys = Config::instance().hotkeys();

    ImGui::TextColored(ImVec4(0.1f, 0.8f, 0.3f, 1.0f), "GPU Screen Recorder");
    ImGui::Text("Version 1.0.0 - Windows Edition");
    ImGui::Spacing();
    ImGui::TextWrapped("A ShadowPlay-like GPU screen recorder for Windows. "
                       "Records your screen with minimal performance impact "
                       "using hardware-accelerated encoding.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Features:");
    ImGui::BulletText("DXGI Desktop Duplication screen capture");
    ImGui::BulletText("Hardware-accelerated encoding (NVENC/AMF/QSV)");
    ImGui::BulletText("Instant replay with ring buffer");
    ImGui::BulletText("Live streaming support");
    ImGui::BulletText("ShadowPlay-style overlay UI");
    ImGui::BulletText("WASAPI audio capture");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("License: GPL-3.0-only");
    ImGui::Text("Powered by FFmpeg, DirectX 11, ImGui");
}

// ─── Stats Overlay (HUD) ────────────────────────────────────────────────────

void UIRenderer::render_stats_overlay() {
    auto& data = m_overlay->overlay_data();
    auto state = m_overlay->recorder_state();

    // Position in top-right corner
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 10, 10),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.65f);

    ImGui::Begin("Stats Overlay", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    ImVec4 color;
    const char* status_text = "";
    switch (state) {
        case RecordingState::Recording:
            color = ImVec4(1.0f, 0.2f, 0.2f, 1.0f);
            status_text = "REC ";
            break;
        case RecordingState::ReplayActive:
            color = ImVec4(0.2f, 0.6f, 1.0f, 1.0f);
            status_text = "REPLAY ";
            break;
        case RecordingState::Streaming:
            color = ImVec4(1.0f, 0.4f, 0.0f, 1.0f);
            status_text = "STREAM ";
            break;
        case RecordingState::Paused:
            color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
            status_text = "PAUSED ";
            break;
        default:
            color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            break;
    }

    ImGui::TextColored(color, "%sFPS: %d  |  %lld frames",
                       status_text, data.fps,
                       static_cast<long long>(data.frames_recorded));

    if (data.recording_duration > 0) {
        int total_sec = static_cast<int>(data.recording_duration);
        int hours = total_sec / 3600;
        int mins = (total_sec % 3600) / 60;
        int secs = total_sec % 60;
        if (hours > 0) {
            ImGui::Text("Time: %d:%02d:%02d", hours, mins, secs);
        } else {
            ImGui::Text("Time: %02d:%02d", mins, secs);
        }
    }

    ImGui::End();
}

// ─── Theme ──────────────────────────────────────────────────────────────────

void UIRenderer::apply_theme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Dark theme inspired by ShadowPlay
    style.WindowPadding = ImVec2(15, 15);
    style.WindowRounding = 8.0f;
    style.FramePadding = ImVec2(8, 6);
    style.FrameRounding = 6.0f;
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = 4.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);

    // Colors (dark green accent theme)
    colors[ImGuiCol_Text] = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.10f, 0.12f, 0.94f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.14f, 0.94f);
    colors[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.22f, 0.50f);
    colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.18f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.27f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.10f, 0.75f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.32f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.37f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.40f, 0.42f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.15f, 0.90f, 0.35f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.18f, 0.20f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.10f, 0.80f, 0.30f, 0.80f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.10f, 0.80f, 0.30f, 0.40f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.10f, 0.80f, 0.30f, 0.60f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.10f, 0.80f, 0.30f, 0.80f);
    colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.20f, 0.22f, 1.0f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.10f, 0.80f, 0.30f, 0.60f);
    colors[ImGuiCol_SeparatorActive] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.10f, 0.80f, 0.30f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.10f, 0.80f, 0.30f, 0.60f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.10f, 0.80f, 0.30f, 0.90f);
    colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.10f, 0.80f, 0.30f, 0.60f);
    colors[ImGuiCol_TabActive] = ImVec4(0.10f, 0.80f, 0.30f, 0.80f);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);
    colors[ImGuiCol_PlotLines] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.15f, 0.90f, 0.35f, 1.0f);
    colors[ImGuiCol_PlotHistogram] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.15f, 0.90f, 0.35f, 1.0f);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(0.10f, 0.80f, 0.30f, 0.35f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.10f, 0.80f, 0.30f, 0.90f);
    colors[ImGuiCol_NavHighlight] = ImVec4(0.10f, 0.80f, 0.30f, 1.0f);
    colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
}
