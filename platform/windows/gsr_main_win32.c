/* gsr_main_win32.c — the gpu-screen-recorder engine executable for Windows
 * (Phase 11, Push 1). Mirrors upstream src/cli/main.c with the platform
 * differences the port documents in docs/upstream-porting-notes.md §3q:
 *
 *   - No POSIX signals: SIGUSR1/SIGRTMIN do not exist on Windows, and the
 *     engine's control channel is the -ipc named pipe (the gsr-ui daemon
 *     and gsr-ui-cli). SIGINT/SIGTERM stop the engine via SetConsoleCtrlHandler,
 *     which fires for Ctrl+C and console close/logoff.
 *   - No display-server environment (DISPLAY/WAYLAND_DISPLAY), no DRM card,
 *     no /proc (install_cuda_no_stable_perf_limit is Linux-only), no geteuid
 *     check, no mallopt.
 *   - The nvidia env vars that matter on Windows (CUDA_DISABLE_PERF_BOOST,
 *     __GL_MaxFramesAllowed, __GL_THREADED_OPTIMIZATIONS) are set with
 *     _putenv_s; the LIBVA/vblank-mode unsets are VAAPI-only and skipped.
 *   - windowing is the Windows implementation (platform/windows/
 *     gsr_windowing_win32.c): no X11 display, EGL is ANGLE-on-D3D11, and
 *     card_path_found means "GL is usable" (set by the EGL loader).
 *   - app audio (GSR_APP_AUDIO) is not built on Windows: -a app:* is
 *     rejected with exit code 2, same as an upstream build without it.
 *
 * Everything else — the -ipc handlers (stop/toggle-pause/set-paused/
 * toggle-replay-recording/start/stop/save-replay), the deferred-request
 * completion on the recording callbacks, the screenshot path, the args
 * validation — is byte-for-byte upstream behavior.
 */
#include "../../upstream/include/cli/commands.h"
#include "../../upstream/include/cli/ipc.h"
#include "../../upstream/include/recorder/recorder.h"
#include "../../upstream/include/recorder/screenshot.h"
#include "../../upstream/include/recorder/capture_source.h"
#include "../../upstream/include/recorder/capture_setup.h"
#include "../../upstream/include/recorder/windowing.h"
#include "../../upstream/include/recorder/audio_input.h"
#include "../../upstream/include/recorder/replay_save.h"
#include "../../upstream/include/recorder/error.h"
#include "../../upstream/include/args_parser.h"
#include "../../upstream/include/sound.h"
#include "../../upstream/include/shader.h"
#include "../../upstream/include/utils.h"
#include "../../upstream/include/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <locale.h>
#include <signal.h>
#include <stdatomic.h>
#include <io.h>    /* _setmode (Windows CRLF fix) */
#include <fcntl.h> /* _O_BINARY            */

#ifdef _WIN32
#include <windows.h>
#endif

static atomic_int running = 1;
static gsr_recorder *recorder = NULL;

#ifdef _WIN32
/* How long a stop request may take before the process force-exits. */
#define GSR_SHUTDOWN_WATCHDOG_MS (30 * 1000)
static LONG shutdown_watchdog_armed = 0;

/* Last-resort guard for a wedged shutdown.
 *
 * The recorder's stop path joins the audio and encode threads; if one of
 * them never returns, the process stays alive forever while the recording
 * has already stopped. That is not just a leak: the process keeps the -ipc
 * named pipe bound, so every later recording started with the same pipe
 * name dies immediately with "another program is already listening", which
 * from the UI looks like "Record keeps stopping and nothing records".
 *
 * Force-exiting releases the pipe. The recording may be left truncated, but
 * a stuck engine must never brick the next one. */
static DWORD WINAPI shutdown_watchdog_thread(LPVOID userdata) {
    (void)userdata;
    Sleep(GSR_SHUTDOWN_WATCHDOG_MS);
    gsr_log(GSR_LOG_LEVEL_ERROR, "the recorder did not stop within %d seconds, forcing exit", GSR_SHUTDOWN_WATCHDOG_MS / 1000);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

static void arm_shutdown_watchdog(void) {
    if(InterlockedCompareExchange(&shutdown_watchdog_armed, 1, 0) != 0)
        return;

    const HANDLE thread = CreateThread(NULL, 0, shutdown_watchdog_thread, NULL, 0, NULL);
    if(thread)
        CloseHandle(thread);
    else
        InterlockedExchange(&shutdown_watchdog_armed, 0);
}
#endif

static void stop_recorder(int signal_number) {
    (void)signal_number;
    atomic_store(&running, 0);
    if(recorder)
        gsr_recorder_stop(recorder);
#ifdef _WIN32
    arm_shutdown_watchdog();
#endif
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch(ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            stop_recorder(0);
            return TRUE;
    }
    return FALSE;
}
#endif

static void install_signal_handlers(void) {
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    /* signal() for SIGINT/SIGTERM still works on MinGW (SIGUSR1/SIGRTMIN do
       not exist on Windows; the IPC pipe covers those control actions). */
    signal(SIGINT, stop_recorder);
    signal(SIGTERM, stop_recorder);
#else
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGUSR1, save_replay_handler);
    signal(SIGUSR2, toggle_pause_handler);
    signal(SIGRTMIN, toggle_replay_recording_handler);
    signal(SIGRTMIN+1, save_replay_10_seconds_handler);
    signal(SIGRTMIN+2, save_replay_30_seconds_handler);
    signal(SIGRTMIN+3, save_replay_1_minute_handler);
    signal(SIGRTMIN+4, save_replay_5_minutes_handler);
    signal(SIGRTMIN+5, save_replay_10_minutes_handler);
    signal(SIGRTMIN+6, save_replay_30_minutes_handler);
#endif
}

static void set_environment_variables(void) {
    /* NVIDIA driver 580.105.08 added CUDA_DISABLE_PERF_BOOST to disable the
       p2 power level issue (cuda/nvenc forces the gpu to p2, decreasing game
       performance). Same values as upstream, via _putenv_s on Windows. */
    _putenv_s("CUDA_DISABLE_PERF_BOOST", "1");
    /* Stop the nvidia driver from buffering frames. */
    _putenv_s("__GL_MaxFramesAllowed", "1");
    /* If set to 1, cuGraphicsGLRegisterImage fails for egl contexts; we
       don't use CUDA interop on Windows (NVENC is d3d11va), but keep the
       same value as upstream to avoid driver surprises. */
    _putenv_s("__GL_THREADED_OPTIMIZATIONS", "0");
}

static bool ipc_stop_handler(char *error_message, size_t error_message_size, void *userdata) {
    (void)error_message;
    (void)error_message_size;
    (void)userdata;
    stop_recorder(0);
    return true;
}

static bool ipc_toggle_pause_handler(char *error_message, size_t error_message_size, void *userdata) {
    const gsr_recorder_settings *settings = userdata;
    if(settings->is_replaying) {
        snprintf(error_message, error_message_size, "pausing is not supported when recording a replay");
        return false;
    }

    gsr_recorder_toggle_pause(recorder);
    return true;
}

static bool ipc_set_paused_handler(bool paused, char *error_message, size_t error_message_size, void *userdata) {
    const gsr_recorder_settings *settings = userdata;
    if(settings->is_replaying) {
        snprintf(error_message, error_message_size, "pausing is not supported when recording a replay");
        return false;
    }

    gsr_recorder_set_paused(recorder, paused);
    return true;
}

static bool ipc_toggle_replay_recording_handler(char *error_message, size_t error_message_size, void *userdata) {
    const gsr_recorder_settings *settings = userdata;
    if(!settings->replay_recording_directory) {
        snprintf(error_message, error_message_size, "option -ro is required to start a recording");
        return false;
    }

    gsr_recorder_toggle_replay_recording(recorder);
    return true;
}

static bool ipc_start_replay_recording_handler(char *error_message, size_t error_message_size, void *userdata) {
    const gsr_recorder_settings *settings = userdata;
    if(!settings->replay_recording_directory) {
        snprintf(error_message, error_message_size, "option -ro is required to start a recording");
        return false;
    }

    gsr_recorder_start_replay_recording(recorder);
    return true;
}

static bool ipc_stop_replay_recording_handler(char *error_message, size_t error_message_size, void *userdata) {
    const gsr_recorder_settings *settings = userdata;
    if(!settings->replay_recording_directory) {
        snprintf(error_message, error_message_size, "option -ro is required to start a recording");
        return false;
    }

    if(!gsr_recorder_is_replay_recording(recorder)) {
        snprintf(error_message, error_message_size, "no recording is running");
        return false;
    }

    gsr_recorder_stop_replay_recording(recorder);
    return true;
}

static bool ipc_save_replay_handler(int seconds, bool has_restart_replay, bool restart_replay, char *error_message, size_t error_message_size, void *userdata) {
    const gsr_recorder_settings *settings = userdata;
    if(!settings->is_replaying) {
        snprintf(error_message, error_message_size, "option -r is required to save a replay");
        return false;
    }

    int restart_replay_request = GSR_RESTART_REPLAY_USE_OPTION;
    if(has_restart_replay)
        restart_replay_request = restart_replay ? GSR_RESTART_REPLAY_ENABLE : GSR_RESTART_REPLAY_DISABLE;

    gsr_recorder_save_replay(recorder, seconds, restart_replay_request);
    return true;
}

static int validate_args_with_capture_sources(args_parser *arg_parser, const gsr_capture_sources *capture_sources) {
    const Arg *output_resolution_arg = args_parser_get_arg(arg_parser, "-s");
    assert(output_resolution_arg);

    const Arg *region_arg = args_parser_get_arg(arg_parser, "-region");
    assert(region_arg);

    if(gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW) && output_resolution_arg->num_values == 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "option -s is required when using '-w focused' option");
        args_parser_print_usage();
        return GSR_ERROR_GENERIC;
    }

    const bool is_capturing_region = gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_REGION);
    if(region_arg->num_values == 0) {
        if(is_capturing_region && !gsr_capture_sources_has_region_set(capture_sources)) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "option -region is required when '-w region' is used");
            args_parser_print_usage();
            return GSR_ERROR_GENERIC;
        }
    } else {
        if(is_capturing_region) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "option -region is deprecated, use -w with region directly instead, for example: -w %s", region_arg->values[0]);
        } else {
            gsr_log(GSR_LOG_LEVEL_ERROR, "option -region can only be used when option '-w region' is used");
            args_parser_print_usage();
            return GSR_ERROR_GENERIC;
        }
    }

    if(!arg_parser->settings.restore_portal_session && gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_PORTAL))
        gsr_log(GSR_LOG_LEVEL_INFO, "option '-w portal' was used without '-restore-portal-session yes'. The previous screencast session will be ignored");

    return GSR_ERROR_OK;
}

static void screenshot_saved_callback(const char *filepath, void *userdata) {
    const char *recording_saved_script = userdata;
    if(recording_saved_script)
        run_recording_saved_script_async(recording_saved_script, filepath, "screenshot");
}

typedef struct {
    const char *recording_saved_script;
    gsr_ipc *ipc;
} recorder_callbacks_context;

static void replay_saved_callback(const char *filepath, void *userdata) {
    recorder_callbacks_context *context = userdata;
    if(!filepath) {
        printf("gsr error: Failed to save replay\n");
        fflush(stdout);
        gsr_ipc_complete_request(context->ipc, GSR_IPC_DEFERRED_REQUEST_SAVE_REPLAY, false, NULL);
        return;
    }

    puts(filepath);
    fflush(stdout);
    if(context->recording_saved_script)
        run_recording_saved_script_async(context->recording_saved_script, filepath, "replay");

    gsr_ipc_complete_request(context->ipc, GSR_IPC_DEFERRED_REQUEST_SAVE_REPLAY, true, filepath);
}

static void recording_started_callback(const char *filepath, void *userdata) {
    (void)userdata;
    if(!filepath) {
        printf("gsr error: Failed to start recording\n");
        fflush(stdout);
    }
}

static void recording_stopped_callback(const char *filepath, void *userdata) {
    recorder_callbacks_context *context = userdata;
    if(!filepath) {
        printf("gsr error: Failed to save recording\n");
        fflush(stdout);
        gsr_ipc_complete_request(context->ipc, GSR_IPC_DEFERRED_REQUEST_STOP_REPLAY_RECORDING, false, NULL);
        return;
    }

    puts(filepath);
    fflush(stdout);
    if(context->recording_saved_script)
        run_recording_saved_script_async(context->recording_saved_script, filepath, "regular");

    gsr_ipc_complete_request(context->ipc, GSR_IPC_DEFERRED_REQUEST_STOP_REPLAY_RECORDING, true, filepath);
}

static int parse_audio_inputs(args_parser *arg_parser, gsr_audio_input_tracks *audio_input_tracks) {
    const Arg *audio_input_arg = args_parser_get_arg(arg_parser, "-a");
    assert(audio_input_arg);

    gsr_audio_devices audio_devices;
    memset(&audio_devices, 0, sizeof(audio_devices));
    if(audio_input_arg->num_values > 0)
        get_pulseaudio_inputs(&audio_devices);

    const int parse_result = gsr_audio_input_tracks_parse(audio_input_tracks, audio_input_arg->values, audio_input_arg->num_values, &audio_devices);
    gsr_audio_devices_deinit(&audio_devices);
    return parse_result;
}

static int take_screenshot(args_parser *arg_parser, gsr_windowing *windowing, gsr_capture_deps *capture_deps, gsr_capture_sources *capture_sources, gsr_image_format image_format) {
    const Arg *plugin_arg = args_parser_get_arg(arg_parser, "-p");
    assert(plugin_arg);

    arg_parser->settings.fps = 60; /* We want to capture an image as soon as possible */

    gsr_screenshot_params screenshot_params;
    memset(&screenshot_params, 0, sizeof(screenshot_params));
    screenshot_params.settings = &arg_parser->settings;
    screenshot_params.egl = &windowing->egl;
    screenshot_params.window = windowing->window;
    screenshot_params.capture_deps = capture_deps;
    screenshot_params.capture_sources = capture_sources;
    screenshot_params.image_format = image_format;
    screenshot_params.plugin_filepaths = plugin_arg->values;
    screenshot_params.num_plugin_filepaths = plugin_arg->num_values;
    screenshot_params.running = &running;
    screenshot_params.screenshot_saved = screenshot_saved_callback;
    screenshot_params.userdata = (void*)arg_parser->settings.recording_saved_script;

    return gsr_screenshot_take(&screenshot_params);
}

static int record(args_parser *arg_parser, gsr_windowing *windowing, gsr_capture_deps *capture_deps, gsr_capture_sources *capture_sources, gsr_audio_input_tracks *audio_input_tracks, gsr_ipc *ipc) {
    const Arg *plugin_arg = args_parser_get_arg(arg_parser, "-p");
    assert(plugin_arg);

    gsr_recorder_params recorder_params;
    memset(&recorder_params, 0, sizeof(recorder_params));
    recorder_params.settings = &arg_parser->settings;
    recorder_params.windowing = windowing;
    recorder_params.capture_deps = capture_deps;
    recorder_params.capture_sources = capture_sources;
    recorder_params.audio_input_tracks = audio_input_tracks;
    recorder_params.plugin_filepaths = plugin_arg->values;
    recorder_params.num_plugin_filepaths = plugin_arg->num_values;

    recorder_callbacks_context callbacks_context;
    callbacks_context.recording_saved_script = arg_parser->settings.recording_saved_script;
    callbacks_context.ipc = ipc;

    gsr_recorder_callbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.replay_saved = replay_saved_callback;
    callbacks.recording_started = recording_started_callback;
    callbacks.recording_stopped = recording_stopped_callback;
    callbacks.userdata = &callbacks_context;

    int error = GSR_ERROR_OK;
    recorder = gsr_recorder_create(&recorder_params, &callbacks, &error);
    if(!recorder)
        return error;

    if(!atomic_load(&running))
        gsr_recorder_stop(recorder);

    gsr_ipc_handlers ipc_handlers;
    memset(&ipc_handlers, 0, sizeof(ipc_handlers));
    ipc_handlers.stop = ipc_stop_handler;
    ipc_handlers.toggle_pause = ipc_toggle_pause_handler;
    ipc_handlers.set_paused = ipc_set_paused_handler;
    ipc_handlers.toggle_replay_recording = ipc_toggle_replay_recording_handler;
    ipc_handlers.start_replay_recording = ipc_start_replay_recording_handler;
    ipc_handlers.stop_replay_recording = ipc_stop_replay_recording_handler;
    ipc_handlers.save_replay = ipc_save_replay_handler;
    ipc_handlers.userdata = &arg_parser->settings;

    int run_result = gsr_ipc_start(ipc, &ipc_handlers);
    if(run_result == GSR_ERROR_OK)
        run_result = gsr_recorder_run(recorder);

    gsr_ipc_complete_request(ipc, GSR_IPC_DEFERRED_REQUEST_STOP, true, arg_parser->settings.is_replaying ? NULL : arg_parser->settings.filename);
    gsr_ipc_stop(ipc);
    gsr_recorder_destroy(recorder, true);
    recorder = NULL;
    return run_result;
}

static int run(args_parser *arg_parser) {
    int exit_code = 0;

    gsr_capture_sources capture_sources;
    gsr_audio_input_tracks audio_input_tracks;
    gsr_windowing windowing;
    gsr_capture_deps capture_deps;
    gsr_ipc ipc;
    memset(&audio_input_tracks, 0, sizeof(audio_input_tracks));
    memset(&windowing, 0, sizeof(windowing));
    memset(&ipc, 0, sizeof(ipc));
    gsr_capture_deps_init(&capture_deps);

    const Arg *ipc_arg = args_parser_get_arg(arg_parser, "-ipc");
    assert(ipc_arg);

    const int parse_capture_sources_result = gsr_capture_sources_parse(&capture_sources, arg_parser->settings.capture_source, arg_parser->settings.region_position, arg_parser->settings.region_size);
    if(parse_capture_sources_result != GSR_ERROR_OK) {
        exit_code = gsr_error_to_exit_code(parse_capture_sources_result);
        goto done;
    }

    if(capture_sources.num_items == 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "option -w can't be empty. You need to capture video from at least one source");
        args_parser_print_usage();
        exit_code = 1;
        goto done;
    }

    const int validate_args_result = validate_args_with_capture_sources(arg_parser, &capture_sources);
    if(validate_args_result != GSR_ERROR_OK) {
        exit_code = gsr_error_to_exit_code(validate_args_result);
        goto done;
    }

    if(ipc_arg->num_values > 0 && gsr_ipc_init(&ipc, ipc_arg->values[0]) != GSR_ERROR_OK) {
        exit_code = 1;
        goto done;
    }

    const int parse_audio_inputs_result = parse_audio_inputs(arg_parser, &audio_input_tracks);
    if(parse_audio_inputs_result != GSR_ERROR_OK) {
        exit_code = gsr_error_to_exit_code(parse_audio_inputs_result);
        goto done;
    }

    const bool uses_app_audio = gsr_audio_input_tracks_has_app_audio(&audio_input_tracks);
    if(uses_app_audio) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "application audio can't be recorded because GPU Screen Recorder is built without application audio support on Windows");
        exit_code = 2;
        goto done;
    }

    gsr_windowing_params windowing_params;
    windowing_params.monitor_capture = gsr_capture_sources_has_monitor_or_region(&capture_sources);
    windowing_params.gl_debug = arg_parser->settings.gl_debug;
    windowing_params.listen_to_x11_events = true;
    if(gsr_windowing_init(&windowing, &windowing_params) != GSR_ERROR_OK) {
        exit_code = 1;
        goto done;
    }

    if(gsr_capture_sources_has_type(&capture_sources, GSR_CAPTURE_SOURCE_TYPE_PORTAL)) {
        if(gsr_windowing_is_using_prime_run()) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "use of prime-run with -w portal option is currently not supported. Disabling prime-run");
            gsr_windowing_disable_prime_run();
        }

        if(video_codec_is_hdr(arg_parser->settings.video_codec)) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "portal capture option doesn't support hdr yet (PipeWire doesn't support hdr), the video will be tonemapped from hdr to sdr");
            arg_parser->settings.video_codec = hdr_video_codec_to_sdr_video_codec(arg_parser->settings.video_codec);
        }
    }

    if(gsr_windowing_load_egl(&windowing, &windowing_params) != GSR_ERROR_OK) {
        exit_code = 1;
        goto done;
    }

    gsr_shader_enable_debug_output(arg_parser->settings.gl_debug);
#ifndef NDEBUG
    gsr_shader_enable_debug_output(true);
#endif

    if(!args_parser_validate_with_gl_info(arg_parser, &windowing.egl)) {
        exit_code = 1;
        goto done;
    }

    if(!windowing.card_path_found) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "no graphics adapter found. Make sure that you have at least one monitor connected");
        exit_code = 2;
        goto done;
    }

    gsr_capture_deps_init_cursor(&capture_deps, &windowing.egl, arg_parser->settings.record_cursor);

    gsr_image_format image_format;
    if(get_image_format_from_filename(arg_parser->settings.filename, &image_format)) {
        if(audio_input_tracks.num_items > 0) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "can't record audio (-a) when taking a screenshot");
            exit_code = 1;
            goto done;
        }

        if(ipc_arg->num_values > 0)
            gsr_log(GSR_LOG_LEVEL_WARNING, "option -ipc has no effect when taking a screenshot");

        exit_code = gsr_error_to_exit_code(take_screenshot(arg_parser, &windowing, &capture_deps, &capture_sources, image_format));
    } else {
        exit_code = gsr_error_to_exit_code(record(arg_parser, &windowing, &capture_deps, &capture_sources, &audio_input_tracks, &ipc));
    }

    done:
    gsr_ipc_deinit(&ipc);
    gsr_capture_deps_deinit(&capture_deps);
    gsr_windowing_deinit(&windowing);
    gsr_audio_input_tracks_deinit(&audio_input_tracks);
    gsr_capture_sources_deinit(&capture_sources);
    return exit_code;
}

int main(int argc, char **argv) {
    /* Windows CRT text mode translates '\n' to "\r\n" when stdout/stderr
       are pipes, which corrupts every line for consumers that expect '\n'
       (the gsr-ui --info/--list-capture-options parsers compare exact
       strings like "vendor|nvidia"). Binary mode keeps the output
       byte-exact on all targets. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    setlocale(LC_ALL, "C"); /* Sigh... stupid C */

    install_signal_handlers();
    set_environment_variables();

    args_handlers arg_handlers;
    arg_handlers.version = version_command;
    arg_handlers.info = info_command;
    arg_handlers.list_audio_devices = list_audio_devices_command;
    arg_handlers.list_application_audio = list_application_audio_command;
    arg_handlers.list_v4l2_devices = list_v4l2_devices;
    arg_handlers.list_capture_options = list_capture_options_command;
    arg_handlers.list_monitors = list_monitors_command;

    args_parser arg_parser;
    int exit_code = 0;
    int command_exit_code = 0;
    switch(args_parser_parse(&arg_parser, argc, argv, &arg_handlers, NULL, &command_exit_code)) {
        case ARGS_PARSE_RESULT_ERROR:
            exit_code = 1;
            break;
        case ARGS_PARSE_RESULT_COMMAND_HANDLED:
            exit_code = command_exit_code;
            break;
        case ARGS_PARSE_RESULT_OK: {
            exit_code = run(&arg_parser);
            break;
        }
    }

    args_parser_deinit(&arg_parser);

    /* Same as upstream: _exit skips atexit handlers (upstream: cuda's at_exit
       can freeze on some drivers; on Windows the recorder threads are joined
       in gsr_recorder_destroy before we get here, so this is belt-and-braces). */
    _exit(exit_code);
}
