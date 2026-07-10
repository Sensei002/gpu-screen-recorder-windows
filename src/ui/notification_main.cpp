// GPU Screen Recorder Notification - Windows Standalone Binary
// A ShadowPlay-style notification overlay for recording events.
// Usage: gsr-notification
//        gsr-notification --show-recording-started
//        gsr-notification --show-recording-stopped --path "C:\Videos\recording.mp4" --duration 120 --size 50000000
//        gsr-notification --show-replay-saved --duration 30
//        gsr-notification --show-screenshot --format png
//        gsr-notification --daemon  (listen on named pipe for remote commands)

#include "ui/notification.h"
#include "common/log.h"
#include "common/config.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <thread>

// ─── CLI Arguments ──────────────────────────────────────────────────────────

struct NotificationArgs {
    bool daemon_mode = false;        // Listen for IPC notifications
    bool show_type_set = false;

    // Direct show mode
    NotificationType type = NotificationType::Info;
    std::string title;
    std::string message;
    std::string subtitle;
    std::string path;
    double duration_sec = 0;
    int64_t file_size = 0;
    int replay_duration = 30;
    std::string screenshot_format;
    std::string stream_url;
};

bool parse_args(int argc, char** argv, NotificationArgs& args) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--daemon" || arg == "-d") {
            args.daemon_mode = true;
        } else if (arg == "--show-recording-started" || arg == "--rec-start") {
            args.type = NotificationType::RecordingStarted;
            args.show_type_set = true;
        } else if (arg == "--show-recording-stopped" || arg == "--rec-stop") {
            args.type = NotificationType::RecordingStopped;
            args.show_type_set = true;
        } else if (arg == "--show-replay-saved" || arg == "--replay") {
            args.type = NotificationType::ReplaySaved;
            args.show_type_set = true;
        } else if (arg == "--show-screenshot" || arg == "--screenshot") {
            args.type = NotificationType::ScreenshotTaken;
            args.show_type_set = true;
        } else if (arg == "--show-streaming-started" || arg == "--stream-start") {
            args.type = NotificationType::StreamingStarted;
            args.show_type_set = true;
        } else if (arg == "--show-streaming-stopped" || arg == "--stream-stop") {
            args.type = NotificationType::StreamingStopped;
            args.show_type_set = true;
        } else if (arg == "--show-error") {
            args.type = NotificationType::Error;
            args.show_type_set = true;
        } else if (arg == "--show-info") {
            args.type = NotificationType::Info;
            args.show_type_set = true;
        } else if (arg == "--title" && i + 1 < argc) {
            args.title = argv[++i];
        } else if (arg == "--message" && i + 1 < argc) {
            args.message = argv[++i];
        } else if (arg == "--subtitle" && i + 1 < argc) {
            args.subtitle = argv[++i];
        } else if (arg == "--path" && i + 1 < argc) {
            args.path = argv[++i];
        } else if (arg == "--duration" && i + 1 < argc) {
            args.duration_sec = std::stod(argv[++i]);
        } else if (arg == "--size" && i + 1 < argc) {
            args.file_size = std::stoll(argv[++i]);
        } else if (arg == "--replay-duration" && i + 1 < argc) {
            args.replay_duration = std::stoi(argv[++i]);
        } else if (arg == "--format" && i + 1 < argc) {
            args.screenshot_format = argv[++i];
        } else if (arg == "--url" && i + 1 < argc) {
            args.stream_url = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            printf("GPU Screen Recorder Notification\n");
            printf("Usage: gsr-notification [options]\n");
            printf("\n");
            printf("Modes:\n");
            printf("  --daemon                     Run in daemon mode (listen for IPC)\n");
            printf("\n");
            printf("Direct show options:\n");
            printf("  --rec-start                  Show recording started notification\n");
            printf("  --rec-stop                   Show recording stopped notification\n");
            printf("  --replay                     Show replay saved notification\n");
            printf("  --screenshot                 Show screenshot taken notification\n");
            printf("  --stream-start               Show streaming started notification\n");
            printf("  --stream-stop                Show streaming stopped notification\n");
            printf("  --show-error                 Show error notification\n");
            printf("  --show-info                  Show info notification\n");
            printf("\n");
            printf("Additional options:\n");
            printf("  --title <text>               Custom title\n");
            printf("  --message <text>             Custom message\n");
            printf("  --subtitle <text>            Custom subtitle\n");
            printf("  --path <path>                File path\n");
            printf("  --duration <sec>             Duration (for recording/streaming)\n");
            printf("  --size <bytes>               File size (for recording)\n");
            printf("  --replay-duration <sec>      Replay duration\n");
            printf("  --format <png|jpg>           Screenshot format\n");
            printf("  --url <url>                  Stream URL\n");
            return false;
        } else if (arg == "-v" || arg == "--version") {
            printf("GPU Screen Recorder Notification v1.0.0\n");
            return false;
        }
    }

    return true;
}

// ─── Daemon Mode ────────────────────────────────────────────────────────────

void run_daemon(std::shared_ptr<NotificationRenderer> renderer) {
    LOG_INFO("Notification daemon running - listening for IPC events");
    
    // In a full implementation, this would:
    // 1. Create a named pipe (e.g., \\.\pipe\GSR-Notification)
    // 2. Listen for incoming Notification protobuf/JSON messages
    // 3. Forward them to the renderer
    // 4. Handle multiple simultaneous clients
    
    // For now, run the renderer loop which also handles queued notifications
    renderer->run();
}

// ─── Main ───────────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)nCmdShow;

    // Parse command line
    int argc = 0;
    LPWSTR* argv_w = CommandLineToArgvW(GetCommandLineW(), &argc);

    // Convert to UTF-8
    std::vector<std::string> args_utf8;
    std::vector<char*> argv_ptrs;
    for (int i = 0; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<size_t>(len) - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv_w[i], -1, &arg[0], len, nullptr, nullptr);
        args_utf8.push_back(std::move(arg));
    }
    LocalFree(argv_w);

    for (auto& a : args_utf8) {
        argv_ptrs.push_back(&a[0]);
    }

    NotificationArgs args;
    if (!parse_args(argc, argv_ptrs.data(), args)) {
        return (argc > 1) ? 0 : 1;
    }

    // Initialize logger
    Logger::instance().set_level(LogLevel::Info);
    Logger::instance().set_file_output(Config::default_config_dir() + "\\gsr-notification.log");

    LOG_INFO("GPU Screen Recorder Notification starting...");

    // Create notification renderer
    auto renderer = std::make_shared<NotificationRenderer>();
    if (!renderer->initialize()) {
        LOG_ERROR("Failed to initialize notification renderer");
        return 1;
    }

    if (args.daemon_mode) {
        // Daemon mode: wait for IPC notifications
        run_daemon(renderer);
    } else if (args.show_type_set) {
        // Direct show: build a notification from CLI args and display it
        Notification notification;

        switch (args.type) {
            case NotificationType::RecordingStarted:
                notification = Notification::recording_started(args.path);
                break;
            case NotificationType::RecordingStopped:
                notification = Notification::recording_stopped(args.path, args.duration_sec, args.file_size);
                break;
            case NotificationType::ReplaySaved:
                notification = Notification::replay_saved(args.path, args.replay_duration);
                break;
            case NotificationType::ScreenshotTaken:
                notification = Notification::screenshot_taken(args.path, args.screenshot_format);
                break;
            case NotificationType::StreamingStarted:
                notification = Notification::streaming_started(args.stream_url);
                break;
            case NotificationType::StreamingStopped:
                notification = Notification::streaming_stopped(args.duration_sec);
                break;
            case NotificationType::Error:
                notification = Notification::error(args.message.empty() ? "An error occurred" : args.message);
                break;
            default:
                notification = Notification::info(
                    args.title.empty() ? "GPU Screen Recorder" : args.title,
                    args.message.empty() ? "Notification" : args.message);
                break;
        }

        // Override with custom fields if provided
        if (!args.title.empty()) notification.title = args.title;
        if (!args.message.empty()) notification.message = args.message;
        if (!args.subtitle.empty()) notification.subtitle = args.subtitle;

        renderer->show_notification(notification);

        // Run the render loop (blocks until notification is done)
        renderer->run();
    } else {
        // No args, show a demo notification
        Notification demo = Notification::info(
            "GPU Screen Recorder",
            "Notification system is running");
        renderer->show_notification(demo);
        renderer->run();
    }

    renderer->shutdown();
    LOG_INFO("GPU Screen Recorder Notification stopped");

    return 0;
}

// ─── Console Entry Point for Debugging ──────────────────────────────────────

int main(int argc, char** argv) {
    // Convert to WinMain-compatible call
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOW);
}
