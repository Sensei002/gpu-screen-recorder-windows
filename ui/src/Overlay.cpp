#include "../include/Overlay.hpp"
#include "../include/Theme.hpp"
#include "../include/Config.hpp"
#include "../include/Process.hpp"
#include "../include/Utils.hpp"
#include "../include/gui/StaticPage.hpp"
#include "../include/gui/DropdownButton.hpp"
#include "../include/gui/CustomRendererWidget.hpp"
#include "../include/gui/SettingsPage.hpp"
#include "../include/gui/ScreenshotSettingsPage.hpp"
#include "../include/gui/GlobalSettingsPage.hpp"
#include "../include/gui/Utils.hpp"
#include "../include/Translation.hpp"
#include "../include/gui/PageStack.hpp"
#include "../include/WindowUtils.hpp"
#include "../include/GlobalHotkeys/GlobalHotkeys.hpp"

#ifdef _WIN32
#include "../include/GlobalHotkeys/GlobalHotkeysWin32.hpp"
#include "../include/CursorTracker/CursorTrackerWin32.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentWin32.hpp"
#include "../include/Clipboard/ClipboardWin32.hpp"
#include "../include/RegionSelector/RegionSelectorWin32.hpp"
#else
#include "../include/DesktopEnvironment/DesktopEnvironmentX11.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentWlroots.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentKde.hpp"
#include "../include/DesktopEnvironment/DesktopEnvironmentGnome.hpp"
#include "../include/GlobalHotkeys/GlobalHotkeysLinux.hpp"
#include "../include/CursorTracker/CursorTrackerX11.hpp"
#include "../include/CursorTracker/CursorTrackerDrm.hpp"
#include "../include/CursorTracker/CursorTrackerHyprland.hpp"
#include "../include/CursorTracker/CursorTrackerNiri.hpp"
#include "../include/CursorTracker/CursorTrackerSway.hpp"
#include "../include/Clipboard/ClipboardX11.hpp"
#include "../include/Clipboard/ClipboardWayland.hpp"
#include "../include/RegionSelector/RegionSelectorX11.hpp"
#include "../include/RegionSelector/RegionSelectorWayland.hpp"
#endif

#include <iomanip>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdexcept>
#include <algorithm>
#include <inttypes.h>
#include <math.h>
/* unistd.h exists on MinGW-w64 and provides read/write/close/access and
   (on gcc 16) usleep; the Windows portability shim supplies kill/waitpid. */
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
/* Windows engine control is delivered over the named-pipe IPC channel
   (gsr-cli), never via kill(): the kill() shim terminates the process
   outright (TerminateProcess) and there are no POSIX signals. Each engine
   instance is started with a fixed -ipc pipe name matching these; only one
   record/replay/stream engine runs at a time in the UI. */
static const char *const GSR_RECORD_IPC_PIPE = "gsr-record";
static const char *const GSR_REPLAY_IPC_PIPE = "gsr-replay";
static const char *const GSR_STREAM_IPC_PIPE = "gsr-stream";
#endif

#ifndef _WIN32
#include <sys/wait.h>
#include <poll.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/shapeconst.h>
#include <X11/Xcursor/Xcursor.h>

#include <wayland-client.h>
#endif

#include <mglpp/system/Rect.hpp>
#include <mglpp/window/Event.hpp>
#include <mglpp/system/Utf8.hpp>

extern "C" {
#include <mgl/mgl.h>
}

namespace gsr {
#ifdef _WIN32
    /* Forward declaration so the engine control helpers are visible from the
       destructor (which runs before the helpers' definitions below). */
    static void send_gsr_control_command(const char *pipe_name, const char *command, const char *arg = nullptr);
    static int wait_gsr_process_exit(pid_t process, int timeout_ms);
#endif
    static const mgl::Color bg_color(0, 0, 0, 100);
    static const double force_window_on_top_timeout_seconds = 1.0;
    static const double replay_status_update_check_timeout_seconds = 1.5;
    static const double replay_saving_notification_timeout_seconds = 0.5;
    static const double short_notification_timeout_seconds = 2.0;
    static const double notification_timeout_seconds = 3.0;
    static const double notification_error_timeout_seconds = 5.0;
    static const double cursor_tracker_update_timeout_sec = 0.1;

#ifndef _WIN32
    static mgl::Texture texture_from_ximage(XImage *img) {
        uint8_t *texture_data = (uint8_t*)malloc(img->width * img->height * 3);
        // TODO:

        for(int y = 0; y < img->height; ++y) {
            for(int x = 0; x < img->width; ++x) {
                unsigned long pixel = XGetPixel(img, x, y);
                unsigned char red = (pixel & img->red_mask) >> 16;
                unsigned char green = (pixel & img->green_mask) >> 8;
                unsigned char blue = pixel & img->blue_mask;

                const size_t texture_data_index = (x + y * img->width) * 3;
                texture_data[texture_data_index + 0] = red;
                texture_data[texture_data_index + 1] = green;
                texture_data[texture_data_index + 2] = blue;
            }
        }

        mgl::Texture texture;
        // TODO:
        texture.load_from_memory(texture_data, img->width, img->height, MGL_IMAGE_FORMAT_RGB);
        free(texture_data);
        return texture;
    }

    static bool texture_from_x11_cursor(XcursorImage *x11_cursor_image, bool *visible, mgl::vec2i *hotspot, mgl::Texture &texture) {
        uint8_t *cursor_data = NULL;
        uint8_t *out = NULL;
        const unsigned int *pixels = NULL;
        *visible = false;

        if(!x11_cursor_image)
            return false;

        if(!x11_cursor_image->pixels)
            return false;

        hotspot->x = x11_cursor_image->xhot;
        hotspot->y = x11_cursor_image->yhot;

        pixels = x11_cursor_image->pixels;
        cursor_data = (uint8_t*)malloc((int)x11_cursor_image->width * (int)x11_cursor_image->height * 4);
        if(!cursor_data)
            return false;

        out = cursor_data;
        /* Un-premultiply alpha */
        for(uint32_t y = 0; y < x11_cursor_image->height; ++y) {
            for(uint32_t x = 0; x < x11_cursor_image->width; ++x) {
                uint32_t pixel = *pixels++;
                uint8_t *in = (uint8_t*)&pixel;
                uint8_t alpha = in[3];
                if(alpha == 0) {
                    alpha = 1;
                } else {
                    *visible = true;
                }

                out[0] = (float)in[2] * 255.0/(float)alpha;
                out[1] = (float)in[1] * 255.0/(float)alpha;
                out[2] = (float)in[0] * 255.0/(float)alpha;
                out[3] = in[3];
                out += 4;
                in += 4;
            }
        }

        texture.load_from_memory(cursor_data, x11_cursor_image->width, x11_cursor_image->height, MGL_IMAGE_FORMAT_RGBA);
        free(cursor_data);
        return true;
    }
#endif /* !_WIN32 */

    static char hex_value_to_str(uint8_t v) {
        if(v <= 9)
            return '0' + v;
        else if(v >= 10 && v <= 15)
            return 'A' + (v - 10);
        else
            return '0';
    }

    // Excludes alpha
    static std::string color_to_hex_str(mgl::Color color) {
        std::string result;
        result.resize(6);

        result[0] = hex_value_to_str((color.r & 0xF0) >> 4);
        result[1] = hex_value_to_str(color.r & 0x0F);

        result[2] = hex_value_to_str((color.g & 0xF0) >> 4);
        result[3] = hex_value_to_str(color.g & 0x0F);

        result[4] = hex_value_to_str((color.b & 0xF0) >> 4);
        result[5] = hex_value_to_str(color.b & 0x0F);

        return result;
    }

    static bool diff_int(int a, int b, int difference) {
        return std::abs(a - b) <= difference;
    }

#ifdef _WIN32
    static bool is_window_fullscreen_on_monitor(Display *display, Window window, const Monitor &monitor) {
        (void)monitor;
        return window_is_fullscreen(display, window);
    }
#else
    static bool is_window_fullscreen_on_monitor(Display *display, Window window, const Monitor &monitor) {
        if(!window)
            return false;

        DrawableGeometry geometry;
        if(!get_drawable_geometry(display, window, &geometry))
            return false;

        const int margin = 2;
        return diff_int(geometry.x, monitor.position.x, margin) && diff_int(geometry.y, monitor.position.y, margin)
            && diff_int(geometry.width, monitor.size.x, margin) && diff_int(geometry.height, monitor.size.y, margin);
    }
#endif

    /*static bool is_window_fullscreen_on_monitor(Display *display, Window window, const mgl_monitor *monitors, int num_monitors) {
        if(!window)
            return false;

        DrawableGeometry geometry;
        if(!get_drawable_geometry(display, window, &geometry))
            return false;

        const int margin = 2;
        for(int i = 0; i < num_monitors; ++i) {
            const mgl_monitor *mon = &monitors[i];
            if(diff_int(geometry.x, mon->pos.x, margin) && diff_int(geometry.y, mon->pos.y, margin)
                && diff_int(geometry.width, mon->size.x, margin) && diff_int(geometry.height, mon->size.y, margin))
            {
                return true;
            }
        }

        return false;
    }*/

#ifndef _WIN32
    static void wait_until_window_viewable(Display *display, Window window, double timeout_seconds) {
        mgl::Clock clock;
        while(clock.get_elapsed_time_seconds() < timeout_seconds) {
            XWindowAttributes window_attr;
            window_attr.map_state = IsUnmapped;
            XGetWindowAttributes(display, window, &window_attr);
            if(window_attr.map_state == IsViewable)
                return;
            usleep(1000);
        }
    }
#endif /* !_WIN32 */

    static const Monitor* find_monitor_at_position(const std::vector<Monitor> &monitors, mgl::vec2i pos) {
        for(const Monitor &monitor : monitors) {
            if(mgl::IntRect(monitor.position, monitor.size).contains(pos))
                return &monitor;
        }
        return nullptr;
    }

    static const Monitor* find_monitor_by_name(const std::vector<Monitor> &monitors, const std::string &name) {
        for(const Monitor &monitor : monitors) {
            if(monitor.name == name)
                return &monitor;
        }
        return nullptr;
    }

    static std::string get_power_supply_online_filepath() {
#ifdef _WIN32
        /* Windows has no sysfs; power state is queried in
           power_supply_is_connected via GetSystemPowerStatus. */
        return "windows-battery";
#else
        std::string result;
        const char *paths[] = {
            "/sys/class/power_supply/ADP0/online",
            "/sys/class/power_supply/ADP1/online",
            "/sys/class/power_supply/AC/online",
            "/sys/class/power_supply/ACAD/online"
        };
        for(const char *power_supply_online_filepath : paths) {
            if(access(power_supply_online_filepath, F_OK) == 0) {
                result = power_supply_online_filepath;
                break;
            }
        }
        return result;
#endif
    }

    static bool power_supply_is_connected(const char *power_supply_online_filepath) {
#ifdef _WIN32
        (void)power_supply_online_filepath;
        SYSTEM_POWER_STATUS status;
        if(!GetSystemPowerStatus(&status))
            return false;
        /* ACLineStatus: 0 = offline (battery), 1 = online (AC), 255 = unknown. */
        return status.ACLineStatus == 1;
#else
        int fd = open(power_supply_online_filepath, O_RDONLY);
        if(fd == -1)
            return false;

        char buf[1];
        const bool is_connected = read(fd, buf, 1) == 1 && buf[0] == '1';
        close(fd);
        return is_connected;
#endif
    }

#ifndef _WIN32
    static bool xinput_is_supported(Display *dpy, int *xi_opcode) {
        *xi_opcode = 0;
        int query_event = 0;
        int query_error = 0;
        if(!XQueryExtension(dpy, "XInputExtension", xi_opcode, &query_event, &query_error)) {
            fprintf(stderr, "gsr-ui error: X Input extension not available\n");
            return false;
        }

        int major = 2;
        int minor = 1;
        int retval = XIQueryVersion(dpy, &major, &minor);
        if (retval != Success) {
            fprintf(stderr, "gsr-ui error: XInput 2.1 is not supported\n");
            return false;
        }

        return true;
    }
#endif /* !_WIN32 */

    static bool are_all_audio_tracks_available_to_capture(const std::vector<AudioTrack> &audio_tracks) {
        const auto audio_devices = get_audio_devices();
        for(const AudioTrack &audio_track : audio_tracks) {
            for(const std::string &audio_input : audio_track.audio_inputs) {
                std::string_view audio_track_name(audio_input.c_str());
                const bool is_app_audio = starts_with(audio_track_name, "app:");
                if(is_app_audio)
                    continue;

                if(starts_with(audio_track_name, "device:"))
                    audio_track_name.remove_prefix(7);

                auto it = std::find_if(audio_devices.begin(), audio_devices.end(), [&](const auto &audio_device) {
                    return audio_device.name == audio_track_name;
                });
                if(it == audio_devices.end()) {
                    //fprintf(stderr, "Audio not ready\n");
                    return false;
                }
            }
        }
        return true;
    }

    static bool is_webcam_available_to_capture(const RecordOptions &record_options) {
        if(record_options.webcam_source.empty())
            return true;

        const auto cameras = get_v4l2_devices();
        for(const GsrCamera &camera : cameras) {
            if(camera.path == record_options.webcam_source)
                return true;
        }
        return false;
    }

    static Hotkey config_hotkey_to_hotkey(ConfigHotkey config_hotkey) {
        return {
            (uint32_t)mgl::Keyboard::key_to_x11_keysym((mgl::Keyboard::Key)config_hotkey.key),
            config_hotkey.modifiers
        };
    }

    static bool bind_linux_hotkeys(GlobalHotkeys *global_hotkeys, Overlay *overlay, bool enable_region_exit) {
        // Returns whether the show/hide hotkey bound: on Windows a failed
        // RegisterHotKey (e.g. the NVIDIA App owns Alt+Z) must be surfaced
        // so the user isn't left with an unreachable hidden UI.
        const bool show_hide_bound = global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().main_config.show_hide_hotkey),
            "toggle_show", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_show();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.start_stop_hotkey),
            "record", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_record(RecordForceType::NONE);
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.pause_unpause_hotkey),
            "pause", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_pause();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.start_stop_region_hotkey),
            "record_region", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_record(RecordForceType::REGION);
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().record_config.start_stop_window_hotkey),
            "record_window", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_record(RecordForceType::WINDOW);
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().streaming_config.start_stop_hotkey),
            "stream", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_stream();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.start_stop_hotkey),
            "replay_start", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->toggle_replay();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.save_hotkey),
            "replay_save", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->save_replay();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.save_1_min_hotkey),
            "replay_save_1_min", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->save_replay_1_min();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().replay_config.save_10_min_hotkey),
            "replay_save_10_min", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->save_replay_10_min();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().screenshot_config.take_screenshot_hotkey),
            "take_screenshot", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->take_screenshot();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().screenshot_config.take_screenshot_region_hotkey),
            "take_screenshot_region", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->take_screenshot_region();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(overlay->get_config().screenshot_config.take_screenshot_window_hotkey),
            "take_screenshot_window", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->take_screenshot_window();
            });

        global_hotkeys->bind_key_press(
            config_hotkey_to_hotkey(ConfigHotkey{ mgl::Keyboard::Key::Escape, HOTKEY_MOD_LCTRL | HOTKEY_MOD_LSHIFT | HOTKEY_MOD_LALT }),
            "exit", [overlay](const std::string &id) {
                fprintf(stderr, "pressed %s\n", id.c_str());
                overlay->go_back_to_old_ui();
            });

        if(enable_region_exit) {
            global_hotkeys->bind_key_press(
                config_hotkey_to_hotkey(ConfigHotkey{ mgl::Keyboard::Key::Escape }),
                "cancel_region_selection", [overlay](const std::string &id) {
                    fprintf(stderr, "pressed %s\n", id.c_str());
                    overlay->cancel_region_selection();
                });
        }

        return show_hide_bound;
    }

#ifndef _WIN32
    static std::unique_ptr<GlobalHotkeys> register_linux_hotkeys(Overlay *overlay, Display *x11_dpy, GlobalHotkeysLinux::GrabType grab_type, bool enable_region_exit) {
        auto global_hotkeys = std::make_unique<GlobalHotkeysLinux>(x11_dpy, grab_type);
        if(!global_hotkeys->start())
            fprintf(stderr, "error: failed to start global hotkeys\n");

        bind_linux_hotkeys(global_hotkeys.get(), overlay, enable_region_exit);
        global_hotkeys->on_gsr_ui_virtual_keyboard_grabbed = [overlay]() {
            overlay->global_hotkeys_ungrab_keyboard = true;
        };
        return global_hotkeys;
    }
#endif /* !_WIN32 */

#ifdef _WIN32
    static std::unique_ptr<GlobalHotkeys> register_win32_hotkeys(Overlay *overlay, bool enable_region_exit) {
        auto global_hotkeys = std::make_unique<GlobalHotkeysWin32>();
        // Records on the Overlay whether the show/hide hotkey (Alt+Z by
        // default) registered. A failed RegisterHotKey (most commonly the
        // NVIDIA App / GeForce Experience overlay owning the same
        // combination) is logged by GlobalHotkeysWin32 and surfaced here:
        // main.cpp force-shows the UI after construction so the user isn't
        // left with an unreachable hidden UI (launch-hide / launch-daemon).
        overlay->show_hide_hotkey_bound = bind_linux_hotkeys(global_hotkeys.get(), overlay, enable_region_exit);
        if(!overlay->show_hide_hotkey_bound) {
            fprintf(stderr, "Error: failed to register the show/hide hotkey - it is likely already in use by another application (e.g. the NVIDIA App or GeForce Experience overlay). The UI will open anyway - change the hotkey in Settings or disable the other application's hotkey.\n");
        }
        return global_hotkeys;
    }
#endif /* _WIN32 */

#ifndef _WIN32
    static std::unique_ptr<GlobalHotkeysJoystick> register_joystick_hotkeys(Overlay *overlay) {
        auto global_hotkeys_js = std::make_unique<GlobalHotkeysJoystick>();
        if(!global_hotkeys_js->start())
            fprintf(stderr, "Warning: failed to start joystick hotkeys\n");

        global_hotkeys_js->bind_action("toggle_show", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->toggle_show();
        });

        global_hotkeys_js->bind_action("save_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->save_replay();
        });

        global_hotkeys_js->bind_action("save_1_min_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->save_replay_1_min();
        });

        global_hotkeys_js->bind_action("save_10_min_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->save_replay_10_min();
        });

        global_hotkeys_js->bind_action("take_screenshot", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->take_screenshot();
        });

        global_hotkeys_js->bind_action("toggle_record", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->toggle_record(RecordForceType::NONE);
        });

        global_hotkeys_js->bind_action("toggle_replay", [overlay](const std::string &id) {
            fprintf(stderr, "pressed %s\n", id.c_str());
            overlay->toggle_replay();
        });

        return global_hotkeys_js;
    }
#endif /* !_WIN32 */

    static NotificationSpeed to_notification_speed(const std::string &notification_speed_str) {
        if(notification_speed_str == "normal")
            return NotificationSpeed::NORMAL;
        else if(notification_speed_str == "fast")
            return NotificationSpeed::FAST;
        else {
            assert(false);
            return NotificationSpeed::NORMAL;
        }
    }

    static pid_t launch_gsr_game_tracker(int *stdout_fd) {
#ifdef _WIN32
        /* gsr-game-tracker is a Linux-only companion binary; not available on
           Windows yet, so the game-start replay feature stays disabled. */
        (void)stdout_fd;
        return -1;
#else
        const bool is_flatpak = getenv("FLATPAK_ID") != nullptr;
        if(is_flatpak) {
            const char *args[] = { "flatpak-spawn", "--host", "--", "/var/lib/flatpak/app/com.dec05eba.gpu_screen_recorder/current/active/files/bin/gsr-game-tracker", NULL };
            return exec_program(args, stdout_fd, false);
        } else {
            const char *args[] = { "gsr-game-tracker", NULL };
            return exec_program(args, stdout_fd, false);
        }
#endif
    }

#ifndef _WIN32
    static int mgl_x_error_handler(Display *display, XErrorEvent *ee) {
        (void)display;
        (void)ee;
        return 0;
    }

    static int mgl_x_io_error_handler(Display *display) {
        (void)display;
        return 0;
    }
#endif /* !_WIN32 */

    Overlay::Overlay(std::string resources_path, GsrInfo gsr_info, SupportedCaptureOptions capture_options, egl_functions egl_funcs, struct wl_display *wayland_dpy) :
        resources_path(std::move(resources_path)),
        gsr_info(std::move(gsr_info)),
        egl_funcs(egl_funcs),
        config(capture_options),
        current_recording_config(capture_options),
        bg_screenshot_overlay({0.0f, 0.0f}),
        top_bar_background({0.0f, 0.0f}),
        close_button_widget({0.0f, 0.0f})
    {
        // wayland_dpy is borrowed from main(); never disconnected here.
        this->wayland_dpy = wayland_dpy;

        gsr_icon_path = this->resources_path + "images/gpu_screen_recorder_logo.png";

        key_bindings[0].key_event.code = mgl::Keyboard::Escape;
        key_bindings[0].key_event.key_states.alt = false;
        key_bindings[0].key_event.key_states.control = false;
        key_bindings[0].key_event.key_states.shift = false;
        key_bindings[0].key_event.key_states.system = false;
        key_bindings[0].callback = [this]() {
            page_stack.pop();
        };

        memset(&window_texture, 0, sizeof(window_texture));

        std::optional<Config> new_config = read_config(capture_options);
        if(new_config)
            config = std::move(new_config.value());

        init_color_theme(config, this->gsr_info);

        power_supply_online_filepath = get_power_supply_online_filepath();
        replay_startup_mode = replay_startup_string_to_type(config.replay_config.turn_on_replay_automatically_mode.c_str());
        set_notification_speed(to_notification_speed(config.main_config.notification_speed));

#ifndef _WIN32
        x11_dpy = XOpenDisplay(nullptr);
        if(x11_dpy) {
            XKeysymToKeycode(x11_dpy, XK_F1); // If we dont call we will never get a MappingNotify
        } else {
            fprintf(stderr, "Warning: XOpenDisplay failed to mapping notify\n");
        }

        if(x11_dpy && mgl_get_context()->display_server_is_wayland) {
            XSetErrorHandler(mgl_x_error_handler);
            XSetIOErrorHandler(mgl_x_io_error_handler);
        }

        if(config.main_config.hotkeys_enable_option == "enable_hotkeys")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::ALL, on_region_selected != nullptr);
        else if(config.main_config.hotkeys_enable_option == "enable_hotkeys_virtual_devices")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::VIRTUAL, on_region_selected != nullptr);
        else if(config.main_config.hotkeys_enable_option == "enable_hotkeys_no_grab")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::NO_GRAB, on_region_selected != nullptr);

        if(config.main_config.joystick_hotkeys_enable_option == "enable_hotkeys")
            global_hotkeys_js = register_joystick_hotkeys(this);

        if(this->gsr_info.system_info.display_server == DisplayServer::X11) {
            cursor_tracker = std::make_unique<CursorTrackerX11>(x11_dpy);
            desktop_environment = std::make_unique<DesktopEnvironmentX11>(x11_dpy);
            supports_window_title = true;
        } else if(this->gsr_info.system_info.display_server == DisplayServer::WAYLAND) {
            const char *xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP");

            const std::string wm_name = x11_dpy ? get_window_manager_name(x11_dpy) : "";
            const bool is_kwin = wm_name == "KWin";
            const bool is_mutter = wm_name.find("GNOME") != std::string::npos;
            const bool is_hyprland = xdg_current_desktop && strstr(xdg_current_desktop, "Hyprland");
            const bool is_niri = xdg_current_desktop && strstr(xdg_current_desktop, "niri");
            const bool is_sway = xdg_current_desktop && strstr(xdg_current_desktop, "sway");

            if(is_hyprland && CursorTrackerHyprland::is_supported())
                cursor_tracker = std::make_unique<CursorTrackerHyprland>();
            else if(is_niri && CursorTrackerNiri::is_supported())
                cursor_tracker = std::make_unique<CursorTrackerNiri>();
            else if(is_sway && CursorTrackerSway::is_supported())
                cursor_tracker = std::make_unique<CursorTrackerSway>();
            else if(!this->gsr_info.gpu_info.card_path.empty())
                cursor_tracker = std::make_unique<CursorTrackerDrm>(this->gsr_info.gpu_info.card_path.c_str(), wayland_dpy);

            if(is_kwin) {
                desktop_environment = std::make_unique<DesktopEnvironmentKde>();
                supports_window_title = true;
            } else if(is_mutter) {
                desktop_environment = std::make_unique<DesktopEnvironmentGnome>(x11_dpy);
                supports_window_title = true;
            } else if(DesktopEnvironmentWlroots::is_supported(wayland_dpy)) {
                desktop_environment = std::make_unique<DesktopEnvironmentWlroots>(wayland_dpy);
                supports_window_title = true;
            } else {
                desktop_environment = std::make_unique<DesktopEnvironmentX11>(x11_dpy);
            }

            if(!config.main_config.wayland_warning_shown) {
                config.main_config.wayland_warning_shown = true;
                save_config(config);
                show_notification(TR("Wayland doesn't support GPU Screen Recorder UI properly, things may not work as expected. Use X11 if you experience issues."), notification_error_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
            }
        }

        if(wayland_dpy && RegionSelectorWayland::is_supported(wayland_dpy))
            region_selector = std::make_unique<RegionSelectorWayland>(wayland_dpy);
        else
            region_selector = std::make_unique<RegionSelectorX11>(x11_dpy);

        if(wayland_dpy && ClipboardWayland::is_supported(wayland_dpy)) {
            clipboard = std::make_unique<ClipboardWayland>(wayland_dpy);
            wayland_native_clipboard = true;
        } else {
            clipboard = std::make_unique<ClipboardX11>();
        }
#else
        /* No X11/Wayland on Windows: null display, native Win32 modules. */
        x11_dpy = nullptr;

        if(config.main_config.hotkeys_enable_option != "disable_hotkeys")
            global_hotkeys = register_win32_hotkeys(this, on_region_selected != nullptr);

        cursor_tracker = std::make_unique<CursorTrackerWin32>();
        desktop_environment = std::make_unique<DesktopEnvironmentWin32>();
        supports_window_title = true;

        region_selector = std::make_unique<RegionSelectorWin32>();
        clipboard = std::make_unique<ClipboardWin32>();
#endif

        desktop_environment->start();
        update_led_indicator_after_settings_change();

#ifndef _WIN32
        gsr_game_tracker_process_id = launch_gsr_game_tracker(&gsr_game_tracker_process_output_fd);
        if(gsr_game_tracker_process_id > 0) {
            const int fdl = fcntl(gsr_game_tracker_process_output_fd, F_GETFL);
            fcntl(gsr_game_tracker_process_output_fd, F_SETFL, fdl | O_NONBLOCK);
            gsr_game_tracker_process_output_file = fdopen(gsr_game_tracker_process_output_fd, "r");
            if(gsr_game_tracker_process_output_file)
                gsr_game_tracker_process_output_fd = -1;
        } else {
            fprintf(stderr, "Warning: failed to launch gsr-game-tracker. The feature to start replay when a game starts will not work\n");
        }
#else
        gsr_game_tracker_process_id = -1;
#endif
    }

    Overlay::~Overlay() {
        hide();

        if(notification_process > 0) {
            kill(notification_process, SIGINT);
            int status;
            if(waitpid(notification_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            notification_process = -1;
        }

        if(gpu_screen_recorder_process > 0) {
#ifdef _WIN32
            /* Graceful stop so the current recording/replay is finalized
               instead of being terminated (which would drop the file). */
            const char *pipe = nullptr;
            if(recording_status == RecordingStatus::REPLAY)
                pipe = GSR_REPLAY_IPC_PIPE;
            else if(recording_status == RecordingStatus::RECORD)
                pipe = GSR_RECORD_IPC_PIPE;
            else if(recording_status == RecordingStatus::STREAM)
                pipe = GSR_STREAM_IPC_PIPE;
            if(pipe)
                send_gsr_control_command(pipe, "stop");
#else
            kill(gpu_screen_recorder_process, SIGINT);
#endif
#ifdef _WIN32
            wait_gsr_process_exit(gpu_screen_recorder_process, 15000);
#else
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
#endif
            gpu_screen_recorder_process = -1;
        }

        if(gpu_screen_recorder_screenshot_process > 0) {
            kill(gpu_screen_recorder_screenshot_process, SIGINT);
            int status;
            if(waitpid(gpu_screen_recorder_screenshot_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            gpu_screen_recorder_screenshot_process = -1;
        }

        if(gsr_game_tracker_process_id > 0) {
            kill(gsr_game_tracker_process_id, SIGINT);
            int status;
            if(waitpid(gsr_game_tracker_process_id, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
            gsr_game_tracker_process_id = -1;
        }

        led_indicator.reset();
        region_selector.reset();
        desktop_environment.reset();

        close_gsr_game_tracker_output();
        close_gpu_screen_recorder_output();
        deinit_color_theme();

#ifndef _WIN32
        if(x11_dpy)
            XCloseDisplay(x11_dpy);
#endif

        // wayland_dpy is borrowed from main() — do not disconnect.
    }

#ifdef _WIN32
    void Overlay::xi_setup() {}
#else
    void Overlay::xi_setup() {
        xi_display = XOpenDisplay(nullptr);
        if(!xi_display) {
            fprintf(stderr, "gsr-ui error: failed to setup XI connection\n");
            return;
        }

        if(!xinput_is_supported(xi_display, &xi_opcode))
            goto error;

        xi_input_xev = (XEvent*)calloc(1, sizeof(XEvent));
        if(!xi_input_xev)
            throw std::runtime_error("gsr-ui error: failed to allocate XEvent data");

        xi_output_xev = (XEvent*)calloc(1, sizeof(XEvent));
        if(!xi_output_xev)
            throw std::runtime_error("gsr-ui error: failed to allocate XEvent data");

        unsigned char mask[XIMaskLen(XI_LASTEVENT)];
        memset(mask, 0, sizeof(mask));
        XISetMask(mask, XI_Motion);
        //XISetMask(mask, XI_RawMotion);
        XISetMask(mask, XI_ButtonPress);
        XISetMask(mask, XI_ButtonRelease);
        XISetMask(mask, XI_KeyPress);
        XISetMask(mask, XI_KeyRelease);

        XIEventMask xi_masks;
        xi_masks.deviceid = XIAllMasterDevices;
        xi_masks.mask_len = sizeof(mask);
        xi_masks.mask = mask;
        if(XISelectEvents(xi_display, DefaultRootWindow(xi_display), &xi_masks, 1) != Success) {
            fprintf(stderr, "gsr-ui error: XISelectEvents failed\n");
            goto error;
        }

        XFlush(xi_display);
        return;

        error:
        free(xi_input_xev);
        xi_input_xev = nullptr;
        free(xi_output_xev);
        xi_output_xev = nullptr;
        if(xi_display) {
            XCloseDisplay(xi_display);
            xi_display = nullptr;
        }
    }

#endif /* !_WIN32 */

    void Overlay::close_gsr_game_tracker_output() {
        if(gsr_game_tracker_process_output_file) {
            fclose(gsr_game_tracker_process_output_file);
            gsr_game_tracker_process_output_file = nullptr;
        }

        if(gsr_game_tracker_process_output_fd > 0) {
            close(gsr_game_tracker_process_output_fd);
            gsr_game_tracker_process_output_fd = -1;
        }
    }

    void Overlay::close_gpu_screen_recorder_output() {
#ifdef _WIN32
        /* On Windows the "fd" is the child's stdout pipe HANDLE. */
        if(gpu_screen_recorder_process_output_fd > 0) {
            CloseHandle((HANDLE)(intptr_t)gpu_screen_recorder_process_output_fd);
            gpu_screen_recorder_process_output_fd = -1;
        }
#else
        if(gpu_screen_recorder_process_output_file) {
            fclose(gpu_screen_recorder_process_output_file);
            gpu_screen_recorder_process_output_file = nullptr;
        }

        if(gpu_screen_recorder_process_output_fd > 0) {
            close(gpu_screen_recorder_process_output_fd);
            gpu_screen_recorder_process_output_fd = -1;
        }
#endif
    }

#ifdef _WIN32
    void Overlay::handle_xi_events() {}
#else
    void Overlay::handle_xi_events() {
        if(!xi_display)
            return;
        // The XInput2 path injects synthesized XEvents into the mgl X11 window. On the
        // native Wayland overlay path the system handle is a wl_egl_window, not a Window,
        // and input arrives through the mgl Wayland event loop directly — skip XI here.
        if(wayland_native_overlay)
            return;

        // The synthesized XEvents target the mgl window, so the display field
        // must be mgl's X11 connection (the one that owns that window).
        Display *display = (Display*)mgl_get_context()->connection;

        while(XPending(xi_display)) {
            XNextEvent(xi_display, xi_input_xev);
            XGenericEventCookie *cookie = &xi_input_xev->xcookie;
            if(cookie->type == GenericEvent && cookie->extension == xi_opcode && XGetEventData(xi_display, cookie)) {
                const XIDeviceEvent *de = (XIDeviceEvent*)cookie->data;
                if(cookie->evtype == XI_Motion) {
                    memset(xi_output_xev, 0, sizeof(*xi_output_xev));
                    xi_output_xev->type = MotionNotify;
                    xi_output_xev->xmotion.display = display;
                    xi_output_xev->xmotion.window = (Window)window->get_system_handle();
                    xi_output_xev->xmotion.subwindow = (Window)window->get_system_handle();
                    xi_output_xev->xmotion.x = de->root_x - window_pos.x;
                    xi_output_xev->xmotion.y = de->root_y - window_pos.y;
                    xi_output_xev->xmotion.x_root = de->root_x;
                    xi_output_xev->xmotion.y_root = de->root_y;
                    //xi_output_xev->xmotion.state = // modifiers // TODO:
                    if(window->inject_x11_event(xi_output_xev, event))
                        on_event(event);
                } else if(cookie->evtype == XI_ButtonPress || cookie->evtype == XI_ButtonRelease) {
                    memset(xi_output_xev, 0, sizeof(*xi_output_xev));
                    xi_output_xev->type = cookie->evtype == XI_ButtonPress ? ButtonPress : ButtonRelease;
                    xi_output_xev->xbutton.display = display;
                    xi_output_xev->xbutton.window = (Window)window->get_system_handle();
                    xi_output_xev->xbutton.subwindow = (Window)window->get_system_handle();
                    xi_output_xev->xbutton.x = de->root_x - window_pos.x;
                    xi_output_xev->xbutton.y = de->root_y - window_pos.y;
                    xi_output_xev->xbutton.x_root = de->root_x;
                    xi_output_xev->xbutton.y_root = de->root_y;
                    //xi_output_xev->xbutton.state = // modifiers // TODO:
                    xi_output_xev->xbutton.button = de->detail;
                    if(window->inject_x11_event(xi_output_xev, event))
                        on_event(event);
                } else if(cookie->evtype == XI_KeyPress || cookie->evtype == XI_KeyRelease) {
                    memset(xi_output_xev, 0, sizeof(*xi_output_xev));
                    xi_output_xev->type = cookie->evtype == XI_KeyPress ? KeyPress : KeyRelease;
                    xi_output_xev->xkey.display = display;
                    xi_output_xev->xkey.window = (Window)window->get_system_handle();
                    xi_output_xev->xkey.subwindow = (Window)window->get_system_handle();
                    xi_output_xev->xkey.x = de->root_x - window_pos.x;
                    xi_output_xev->xkey.y = de->root_y - window_pos.y;
                    xi_output_xev->xkey.x_root = de->root_x;
                    xi_output_xev->xkey.y_root = de->root_y;
                    xi_output_xev->xkey.state = de->mods.effective;
                    xi_output_xev->xkey.keycode = de->detail;
                    if(window->inject_x11_event(xi_output_xev, event))
                        on_event(event);
                }
                //fprintf(stderr, "got xi event: %d\n", cookie->evtype);
                XFreeEventData(xi_display, cookie);
            }
        }
    }

#endif /* !_WIN32 */

    static uint32_t key_event_to_bitmask(mgl::Event::KeyEvent key_event) {
        return ((uint32_t)key_event.key_states.alt     << (uint32_t)0)
            |  ((uint32_t)key_event.key_states.control << (uint32_t)1)
            |  ((uint32_t)key_event.key_states.shift   << (uint32_t)2)
            |  ((uint32_t)key_event.key_states.system  << (uint32_t)3);
    }

    void Overlay::process_key_bindings(mgl::Event &event) {
        if(event.type != mgl::Event::KeyReleased)
            return;

        const uint32_t event_key_bitmask = key_event_to_bitmask(event.key);
        for(const KeyBinding &key_binding : key_bindings) {
            if(event.key.code == key_binding.key_event.code && event_key_bitmask == key_event_to_bitmask(key_binding.key_event))
                key_binding.callback();
        }
    }

    void Overlay::stop_region_selection() {
        if(on_region_selected) {
            on_region_selected = nullptr;
            rebind_all_keyboard_hotkeys();
        }
    }

#ifdef _WIN32
    void Overlay::handle_x11_events() {}
#else
    void Overlay::handle_x11_events() {
        if(!x11_dpy)
            return;

        bool mapping_updated = false;
        while(XPending(x11_dpy)) {
            XNextEvent(x11_dpy, &x11_xev);
            switch(x11_xev.type) {
                case MappingNotify: {
                    XRefreshKeyboardMapping(&x11_xev.xmapping);
                    mapping_updated = true;
                    break;
                }
            }
            region_selector->handle_event(&x11_xev);
        }

        if(mapping_updated)
            rebind_all_keyboard_hotkeys();
    }

#endif /* !_WIN32 */

#ifdef _WIN32
    void Overlay::handle_wayland_events() {}
#else
    void Overlay::handle_wayland_events() {
        if(!wayland_dpy)
            return;

        while(wl_display_prepare_read(wayland_dpy) != 0) {
            wl_display_dispatch_pending(wayland_dpy);
        }
        wl_display_flush(wayland_dpy);

        struct pollfd pfd = { wl_display_get_fd(wayland_dpy), POLLIN, 0 };
        if(poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            wl_display_read_events(wayland_dpy);
            wl_display_dispatch_pending(wayland_dpy);
        } else {
            wl_display_cancel_read(wayland_dpy);
        }

        region_selector->handle_event(nullptr);
    }

#endif /* !_WIN32 */

    void Overlay::handle_events() {
        if(led_indicator)
            led_indicator->update();

        if(global_hotkeys_ungrab_keyboard) {
            global_hotkeys_ungrab_keyboard = false;
            show_notification(
                TR("Some keyboard remapping software conflicts with GPU Screen Recorder on your system.\n"
                "Keyboards have been ungrabbed, applications will now receive the hotkeys you press.")
                , 7.0, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);

            config.main_config.hotkeys_enable_option = "enable_hotkeys_no_grab";
            save_config(config);
            recreate_global_hotkeys("enable_hotkeys_no_grab");
        }

        if(global_hotkeys)
            global_hotkeys->poll_events();

#ifndef _WIN32
        if(global_hotkeys_js)
            global_hotkeys_js->poll_events();
#endif

        if(cursor_tracker_update_clock.get_elapsed_time_seconds() >= cursor_tracker_update_timeout_sec) {
            cursor_tracker_update_clock.restart();
            if(cursor_tracker)
                cursor_tracker->update();
        }

        desktop_environment->update();
        handle_x11_events();
        handle_wayland_events();

        if(region_selector->take_canceled()) {
            stop_region_selection();
        } else if(region_selector->take_selection() && on_region_selected) {
            switch(region_selector->get_selection_type()) {
                case RegionSelector::SelectionType::NONE: {
                    break;
                }
                case RegionSelector::SelectionType::REGION: {
                    on_region_selected();
                    break;
                }
                case RegionSelector::SelectionType::WINDOW: {
#ifndef _WIN32
                    Display *display = x11_dpy;

                    const Window selected_window = region_selector->get_window_selection();
                    if(selected_window && selected_window != DefaultRootWindow(display)) {
                        on_region_selected();
                    } else {
                        show_notification(TR("No window selected"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                    }
#else
                    const Window selected_window = region_selector->get_window_selection();
                    if(selected_window) {
                        on_region_selected();
                    } else {
                        show_notification(TR("No window selected"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                    }
#endif
                    break;
                }
            }
            stop_region_selection();
        }

        if(!visible || !window)
            return;

        handle_xi_events();

        while(window->poll_event(event)) {
            if(global_hotkeys) {
                if(!global_hotkeys->on_event(event))
                    continue;
            }
            on_event(event);
        }
    }

    void Overlay::on_event(mgl::Event &event) {
        if(!visible || !window)
            return;

        if(!close_button_widget.on_event(event, *window, mgl::vec2f(0.0f, 0.0f)))
            return;

        if(!page_stack.on_event(event, *window, mgl::vec2f(0.0f, 0.0f)))
            return;

        process_key_bindings(event);
    }

    bool Overlay::draw() {
        remove_widgets_to_be_removed();

        if(reload_ui) {
            const bool reopen_settings = reopen_settings_after_reload;
            const int settings_scroll_y = pending_settings_scroll_y;
            reload_ui = false;
            reopen_settings_after_reload = false;
            pending_settings_scroll_y = 0;
            if(visible) {
                recreate_frontpage_ui_components();
                if(reopen_settings)
                    open_settings_page(settings_scroll_y);
            }
        }

        update_notification_process_status();
        process_gsr_game_tracker_output();
        process_gsr_output();
        update_gsr_process_status();
        update_gsr_screenshot_process_status();
        replay_status_update_status();

        update_gsr_game_tracker_replay_status();

        if(hide_ui) {
            hide_ui = false;
            hide();
            return false;
        }

        if(start_region_capture) {
            start_region_capture = false;
            hide();
            rebind_all_keyboard_hotkeys();
            if(!region_selector->start(RegionSelector::SelectionType::REGION, get_color_theme().tint_color)) {
                show_notification(TR("Failed to start region capture"), notification_error_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                stop_region_selection();
            }
        }

        if(start_window_capture) {
            start_window_capture = false;
            hide();
            rebind_all_keyboard_hotkeys();
            if(!region_selector->start(RegionSelector::SelectionType::WINDOW, get_color_theme().tint_color)) {
                show_notification(TR("Failed to start window capture"), notification_error_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
                stop_region_selection();
            }
        }

        if(region_selector->is_started()) {
            usleep(5 * 1000); // 5 ms
            return true;
        }

        if(!visible)
            return false;

        if(page_stack.empty()) {
            hide();
            return false;
        }

        if(!window)
            return false;

        grab_mouse_and_keyboard();

        //force_window_on_top();

        const bool draw_ui = show_overlay_clock.get_elapsed_time_seconds() >= show_overlay_timeout_seconds;

        window->clear(draw_ui ? bg_color : mgl::Color(0, 0, 0, 0));

        if(draw_ui) {
            if(window_texture_sprite.get_texture() && window_texture.texture_id) {
                window->draw(window_texture_sprite);
                window->draw(bg_screenshot_overlay);
            } else if(screenshot_texture.is_valid()) {
                window->draw(screenshot_sprite);
                window->draw(bg_screenshot_overlay);
            }

            window->draw(top_bar_background);
            window->draw(top_bar_text);
            window->draw(logo_sprite);

            close_button_widget.draw(*window, mgl::vec2f(0.0f, 0.0f));
            page_stack.draw(*window, mgl::vec2f(0.0f, 0.0f));
            draw_tooltip(*window);

            if(cursor_texture.is_valid()) {
                cursor_sprite.set_position((window->get_mouse_position() - cursor_hotspot).to_vec2f());
                window->draw(cursor_sprite);
            }

            if(!drawn_first_frame) {
                drawn_first_frame = true;
                mgl::Event event;
                event.type = mgl::Event::MouseMoved;
                event.mouse_move.x = window->get_mouse_position().x;
                event.mouse_move.y = window->get_mouse_position().y;
                on_event(event);
            }
        }

        window->display();

        return true;
    }

#ifdef _WIN32
    void Overlay::grab_mouse_and_keyboard() {}
#else
    void Overlay::grab_mouse_and_keyboard() {
        // TODO: Remove these grabs when debugging with a debugger, or your X11 session will appear frozen.
        // There should be a debug mode to not use these
        // Layer-shell takes care of input routing via keyboard_interactivity; X11 grabs
        // don't apply to a wl_egl_window.
        if(wayland_native_overlay)
            return;
        // XGrabPointer/XGrabKeyboard need mgl's X11 connection — the one that owns
        // the mgl window — otherwise the X server rejects with BadWindow.
        Display *display = (Display*)mgl_get_context()->connection;
        XGrabPointer(display, (Window)window->get_system_handle(), True,
            ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
            Button1MotionMask | Button2MotionMask | Button3MotionMask | Button4MotionMask | Button5MotionMask |
            ButtonMotionMask,
            GrabModeAsync, GrabModeAsync, None, default_cursor, CurrentTime);
        // TODO: This breaks global hotkeys (when using x11 global hotkeys)
        XGrabKeyboard(display, (Window)window->get_system_handle(), True, GrabModeAsync, GrabModeAsync, CurrentTime);
        XFlush(display);
    }

#endif /* !_WIN32 */

#ifdef _WIN32
    void Overlay::xi_setup_fake_cursor() {}
#else
    void Overlay::xi_setup_fake_cursor() {
        if(!xi_display)
            return;

        XFixesHideCursor(xi_display, DefaultRootWindow(xi_display));
        XFlush(xi_display);

        // TODO: XCURSOR_SIZE and XCURSOR_THEME environment variables
        const char *cursor_theme = XcursorGetTheme(xi_display);
        if(!cursor_theme) {
            //fprintf(stderr, "Warning: failed to get cursor theme, using \"default\" theme instead\n");
            cursor_theme = "default";
        }

        int cursor_size = XcursorGetDefaultSize(xi_display);
        if(cursor_size <= 1)
            cursor_size = 24;

        XcursorImage *cursor_image = nullptr;
        for(int cursor_size_test : {cursor_size, 24}) {
            for(const char *cursor_theme_test : {cursor_theme, "default", "Adwaita"}) {
                for(unsigned int shape : {XC_left_ptr, XC_arrow}) {
                    cursor_image = XcursorShapeLoadImage(shape, cursor_theme_test, cursor_size_test);
                    if(cursor_image)
                        goto done;
                }
            }
        }

        done:
        if(!cursor_image) {
            fprintf(stderr, "Error: failed to get cursor, loading bundled default cursor instead\n");
            const std::string default_cursor_path = resources_path + "images/default.cur";
            for(int cursor_size_test : {cursor_size, 24}) {
                cursor_image = XcursorFilenameLoadImage(default_cursor_path.c_str(), cursor_size_test);
                if(cursor_image)
                    break;
            }
        }

        if(!cursor_image) {
            fprintf(stderr, "Error: failed to get cursor\n");
            XFixesShowCursor(xi_display, DefaultRootWindow(xi_display));
            XFlush(xi_display);
            return;
        }

        bool cursor_visible = false;
        texture_from_x11_cursor(cursor_image, &cursor_visible, &cursor_hotspot, cursor_texture);
        if(cursor_texture.is_valid())
            cursor_sprite.set_texture(&cursor_texture);

        XcursorImageDestroy(cursor_image);
    }

#endif /* !_WIN32 */

    void Overlay::show() {
        if(visible)
            return;

        if(region_selector->is_started())
            return;

        drawn_first_frame = false;
        window.reset();
        window = std::make_unique<mgl::Window>();
        deinit_theme();

        Display *display = x11_dpy;

        const std::string wm_name = get_window_manager_name(display);
        const bool is_kwin = wm_name == "KWin";
        const bool is_wlroots = wm_name.find("wlroots") != std::string::npos;
        // On compositors where override-redirect X11 doesn't work and that advertise wlr-layer-shell
        wayland_native_overlay = is_wayland_layer_shell_overlay_session();

        const std::vector<Monitor> monitors = wayland_native_overlay ? get_monitors_wayland(wayland_dpy) : get_monitors(display);
        if(monitors.empty()) {
            fprintf(stderr, "gsr warning: no monitors found, not showing overlay\n");
            window.reset();
            return;
        }

        std::optional<CursorInfo> cursor_info;
        if(cursor_tracker) {
            cursor_tracker->update();
            cursor_info = cursor_tracker->get_latest_cursor_info();
        }

        // The cursor position is wrong on wayland if an x11 window is not focused. On wayland we instead create a window and get the position where the wayland compositor puts it
        Window x11_cursor_window = 0;
        mgl::vec2i cursor_position = get_cursor_position(display, &x11_cursor_window);
        const Monitor *focused_monitor = nullptr;
        if(cursor_info) {
            focused_monitor = find_monitor_by_name(monitors, cursor_info->monitor_name);
            if(!focused_monitor)
                focused_monitor = &monitors.front();
        } else {
            const mgl::vec2i monitor_position_query_value = (x11_cursor_window || gsr_info.system_info.display_server != DisplayServer::WAYLAND) ? cursor_position : create_window_get_center_position(display);
            focused_monitor = find_monitor_at_position(monitors, monitor_position_query_value);
            if(!focused_monitor)
                focused_monitor = &monitors.front();
        }

        // Wayland doesn't allow XGrabPointer/XGrabKeyboard when a wayland application is focused.
        // If the focused window is a wayland application then don't use override redirect and instead create
        // a fullscreen window for the ui.
        const Window x11_focused_window = get_focused_window(display, WindowCaptureType::FOCUSED, false);
        const bool prevent_game_minimizing = gsr_info.system_info.display_server != DisplayServer::WAYLAND
            || (x11_focused_window && is_window_fullscreen_on_monitor(display, x11_focused_window, *focused_monitor))
            || is_wlroots
            || wayland_native_overlay;

        const bool drm_cursor_pos = (!prevent_game_minimizing || is_wlroots) && cursor_info;
        if(drm_cursor_pos)
            cursor_position = cursor_info->position;

        if(prevent_game_minimizing || wayland_native_overlay) {
            window_pos = focused_monitor->position;
            window_size = focused_monitor->size;
        } else {
            window_pos = {0, 0};
            window_size = focused_monitor->size / 2;
        }

        mgl::Window::CreateParams window_create_params;
        window_create_params.size = wayland_native_overlay ? mgl::vec2i(0, 0) : window_size;
        if(prevent_game_minimizing) {
            window_create_params.min_size = window_size;
            window_create_params.max_size = window_size;
        }
        window_create_params.position = focused_monitor->position + focused_monitor->size / 2 - window_size / 2;
        window_create_params.hidden = true;
        window_create_params.override_redirect = prevent_game_minimizing;
        window_create_params.background_color = mgl::Color(0, 0, 0, 0);
        window_create_params.support_alpha = true;
        window_create_params.hide_decorations = true;
        if(wayland_native_overlay) {
            // wlr-layer-shell OVERLAY surface — full-monitor, no decorations, native Wayland.
            window_create_params.window_type = MGL_WINDOW_TYPE_OVERLAY;
            window_create_params.layer_shell_options.keyboard_interactivity = MGL_LAYER_SHELL_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
            window_create_params.layer_shell_options.exclusive_zone = -1;
        } else {
            // MGL_WINDOW_TYPE_DIALOG is needed for kde plasma wayland in some cases, otherwise the window will pop up on another activity
            // or may not be visible at all
            window_create_params.window_type = (is_kwin && gsr_info.system_info.display_server == DisplayServer::WAYLAND) ? MGL_WINDOW_TYPE_DIALOG : MGL_WINDOW_TYPE_NORMAL;
        }
        // Nvidia + Wayland + Egl doesn't work on some systems properly and it instead falls back to software rendering.
        // Use Glx on Wayland to workaround this issue. This is fine since Egl is only needed for x11 to reliably get the texture of the fullscreen window on Nvidia
        // when a compositor isn't running.
        // Layer-shell requires a native Wayland surface, which means EGL — the GLX workaround above doesn't apply to it.
#ifndef _WIN32
        if(wayland_native_overlay)
            window_create_params.graphics_api = MGL_GRAPHICS_API_EGL;
        else
            window_create_params.graphics_api = gsr_info.system_info.display_server == DisplayServer::WAYLAND ? MGL_GRAPHICS_API_GLX : MGL_GRAPHICS_API_EGL;
#else
        window_create_params.graphics_api = MGL_GRAPHICS_API_WGL;
#endif
        window_create_params.class_name = "gsr-ui";

        if(!window->create("gsr ui", window_create_params)) {
            fprintf(stderr, "error: failed to create window\n");
            window.reset();
            return;
        }
        //window->set_low_latency(true);

#ifndef _WIN32
        if(!wayland_native_overlay) {
            // Properties set on the mgl window must use mgl's X11 connection.
            Display *mgl_display = (Display*)mgl_get_context()->connection;
            unsigned char data = 2; // Prefer being composed to allow transparency
            XChangeProperty(mgl_display, (Window)window->get_system_handle(), XInternAtom(mgl_display, "_NET_WM_BYPASS_COMPOSITOR", False), XA_CARDINAL, 32, PropModeReplace, &data, 1);

            data = 1;
            XChangeProperty(mgl_display, (Window)window->get_system_handle(), XInternAtom(mgl_display, "GAMESCOPE_EXTERNAL_OVERLAY", False), XA_CARDINAL, 32, PropModeReplace, &data, 1);
        }
#endif

        const auto original_window_size = window_size;
        window_pos = focused_monitor->position;
        window_size = wayland_native_overlay ? window->get_size() : focused_monitor->size;
        if(!init_theme(resources_path)) {
            fprintf(stderr, "Error: failed to load theme\n");
            window.reset();
            return;
        }
        get_theme().set_window_size(window_size);

        if(prevent_game_minimizing && !wayland_native_overlay) {
            window->set_size(window_size);
            window->set_size_limits(window_size, window_size);
        }
        if(!wayland_native_overlay)
            window->set_position(focused_monitor->position + focused_monitor->size / 2 - original_window_size / 2);

        mgl_window *win = window->internal_window();
        win->cursor_position.x = cursor_position.x - window_pos.x;
        win->cursor_position.y = cursor_position.y - window_pos.y;

        update_compositor_texture(*focused_monitor);

        visible = true;
        recreate_frontpage_ui_components();

        // The focused application can be an xwayland application but the cursor can hover over a wayland application.
        // This is even the case when hovering over the titlebar of the xwayland application.
        const bool fake_cursor = !wayland_native_overlay && (is_wlroots ? x11_cursor_window != 0 : prevent_game_minimizing);
        if(fake_cursor)
            xi_setup();

        if(!wayland_native_overlay && gsr_info.system_info.display_server == DisplayServer::X11)
            make_window_click_through((Display*)mgl_get_context()->connection, (Window)window->get_system_handle());

        if(!is_wlroots)
            window->set_fullscreen(true);

        window->set_visible(true);
#ifndef _WIN32
        if(!wayland_native_overlay && !prevent_game_minimizing)
            wait_until_window_viewable((Display*)mgl_get_context()->connection, (Window)window->get_system_handle(), 0.5);
#endif

        if(!wayland_native_overlay) {
            // All ops below operate on the mgl window — use mgl's X11 connection.
            Display *mgl_display = (Display*)mgl_get_context()->connection;
            make_window_sticky(mgl_display, (Window)window->get_system_handle());
            hide_window_from_taskbar(mgl_display, (Window)window->get_system_handle());

#ifndef _WIN32
            if(default_cursor) {
                XFreeCursor(mgl_display, default_cursor);
                default_cursor = 0;
            }
            default_cursor = XCreateFontCursor(mgl_display, XC_left_ptr);
            XFlush(mgl_display);
#endif
        }

        grab_mouse_and_keyboard();

        // The real cursor doesn't move when all devices are grabbed, so we create our own cursor and diplay that while grabbed
        cursor_hotspot = {0, 0};
        xi_setup_fake_cursor();
        if(drm_cursor_pos) {
            win->cursor_position.x += cursor_hotspot.x;
            win->cursor_position.y += cursor_hotspot.y;
        }

        // We want to grab all devices to prevent any other application below the UI from receiving events.
        // Owlboy seems to use xi events and XGrabPointer doesn't prevent owlboy from receiving events.
        xi_grab_all_mouse_devices(xi_display);

        show_overlay_timeout_seconds = 0.0;
        show_overlay_clock.restart();

        // Dumb wayland glitch workaround when fullscreening window
        if(!wayland_native_overlay && !prevent_game_minimizing) {
            mgl::Clock resize_wait_clock;
            while(resize_wait_clock.get_elapsed_time_seconds() < 0.5) {
                const mgl::vec2i current_window_size = window->get_size();
                if(current_window_size.x == window_size.x && current_window_size.y == window_size.y)
                    break;
                handle_events();
                if(!visible || !window)
                    return;
                usleep(1000);
            }
        }

        draw();
    }

    void Overlay::recreate_global_hotkeys(std::string_view hotkey_option) {
        global_hotkeys.reset();
#ifndef _WIN32
        if(hotkey_option == "enable_hotkeys")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::ALL, on_region_selected != nullptr);
        else if(hotkey_option == "enable_hotkeys_virtual_devices")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::VIRTUAL, on_region_selected != nullptr);
        else if(hotkey_option == "enable_hotkeys_no_grab")
            global_hotkeys = register_linux_hotkeys(this, x11_dpy, GlobalHotkeysLinux::GrabType::NO_GRAB, on_region_selected != nullptr);
        else if(hotkey_option == "disable_hotkeys")
            global_hotkeys.reset();
#else
        if(hotkey_option != "disable_hotkeys")
            global_hotkeys = register_win32_hotkeys(this, on_region_selected != nullptr);
#endif
    }

    void Overlay::update_led_indicator_after_settings_change() {
        if(config.record_config.record_options.use_led_indicator || config.replay_config.record_options.use_led_indicator || config.streaming_config.record_options.use_led_indicator || config.screenshot_config.use_led_indicator) {
            if(!led_indicator)
                led_indicator = std::make_unique<LedIndicator>();
        } else {
            led_indicator.reset();
        }
    }

    void Overlay::recreate_frontpage_ui_components() {
        bg_screenshot_overlay = mgl::Rectangle(mgl::vec2f(get_theme().window_width, get_theme().window_height));
        top_bar_background = mgl::Rectangle(mgl::vec2f(get_theme().window_width, get_theme().window_height*0.06f).floor());
        top_bar_text = mgl::Text("GPU Screen Recorder", get_theme().top_bar_font_desc.c_str());
        logo_sprite = mgl::Sprite(&get_theme().logo_texture);
        close_button_widget.set_size(mgl::vec2f(top_bar_background.get_size().y * 0.35f, top_bar_background.get_size().y * 0.35f).floor());

        bg_screenshot_overlay.set_color(bg_color);
        top_bar_background.set_color(mgl::Color(0, 0, 0, 180));
        //top_bar_text.set_color(get_color_theme().tint_color);
        top_bar_text.set_position((top_bar_background.get_position() + top_bar_background.get_size()*0.5f - top_bar_text.get_bounds().size*0.5f).floor());

        logo_sprite.set_height((int)(top_bar_background.get_size().y * 0.65f));
        logo_sprite.set_position(mgl::vec2f(
            (top_bar_background.get_size().y - logo_sprite.get_size().y) * 0.5f,
            top_bar_background.get_size().y * 0.5f - logo_sprite.get_size().y * 0.5f
        ).floor());

        close_button_widget.set_position(mgl::vec2f(get_theme().window_width - close_button_widget.get_size().x - logo_sprite.get_position().x, top_bar_background.get_size().y * 0.5f - close_button_widget.get_size().y * 0.5f).floor());

        while(!page_stack.empty()) {
            page_stack.pop();
        }

        auto front_page = std::make_unique<StaticPage>(window_size.to_vec2f());
        StaticPage *front_page_ptr = front_page.get();
        page_stack.push(std::move(front_page));

        const int button_height = window_size.y / 5.0f;
        const int button_width = button_height;

        auto main_buttons_list = std::make_unique<List>(List::Orientation::HORIZONTAL);
        List * main_buttons_list_ptr = main_buttons_list.get();
        main_buttons_list->set_spacing(0.0f);
        {
            auto button = std::make_unique<DropdownButton>(get_theme().title_font_desc.c_str(), get_theme().body_font_desc.c_str(), TR("Instant Replay"), TR("Off"), &get_theme().replay_button_texture,
                mgl::vec2f(button_width, button_height));
            replay_dropdown_button_ptr = button.get();
            button->add_item(TR("Turn on"), "start", config.replay_config.start_stop_hotkey.to_string(false, false));
            button->add_item(TR("Save"), "save", config.replay_config.save_hotkey.to_string(false, false));
            button->add_item(TR("Save 1 min"), "save_1_min", config.replay_config.save_1_min_hotkey.to_string(false, false));
            button->add_item(TR("Save 10 min"), "save_10_min", config.replay_config.save_10_min_hotkey.to_string(false, false));
            button->add_item(TR("Settings"), "settings");
            button->set_item_icon("start", &get_theme().play_texture);
            button->set_item_icon("save", &get_theme().save_texture);
            button->set_item_icon("save_1_min", &get_theme().save_texture);
            button->set_item_icon("save_10_min", &get_theme().save_texture);
            button->set_item_icon("settings", &get_theme().settings_extra_small_texture);
            button->on_click = [this](const std::string &id) {
                if(id == "settings") {
                    auto replay_settings_page = std::make_unique<SettingsPage>(SettingsPage::Type::REPLAY, &gsr_info, config, &page_stack, supports_window_title);
                    replay_settings_page->on_config_changed = [this]() {
                        replay_startup_mode = replay_startup_string_to_type(config.replay_config.turn_on_replay_automatically_mode.c_str());
                        if(recording_status == RecordingStatus::REPLAY)
                            show_notification(TR("Replay settings have been modified. You may need to restart replay to apply the changes."), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);
                    };
                    page_stack.push(std::move(replay_settings_page));
                } else if(id == "save") {
                    on_press_save_replay();
                } else if(id == "save_1_min") {
                    on_press_save_replay_1_min_replay();
                } else if(id == "save_10_min") {
                    on_press_save_replay_10_min_replay();
                } else if(id == "start") {
                    on_press_start_replay(false, false, true);
                }
            };
            button->set_item_enabled("save", false);
            button->set_item_enabled("save_1_min", false);
            button->set_item_enabled("save_10_min", false);
            main_buttons_list->add_widget(std::move(button));
        }
        {
            auto button = std::make_unique<DropdownButton>(get_theme().title_font_desc.c_str(), get_theme().body_font_desc.c_str(), TR("Record"), TR("Not recording"), &get_theme().record_button_texture,
                mgl::vec2f(button_width, button_height));
            record_dropdown_button_ptr = button.get();
            button->add_item(TR("Start"), "start", config.record_config.start_stop_hotkey.to_string(false, false));
            button->add_item(TR("Pause"), "pause", config.record_config.pause_unpause_hotkey.to_string(false, false));
            button->add_item(TR("Settings"), "settings");
            button->set_item_icon("start", &get_theme().play_texture);
            button->set_item_icon("pause", &get_theme().pause_texture);
            button->set_item_icon("settings", &get_theme().settings_extra_small_texture);
            button->on_click = [this](const std::string &id) {
                if(id == "settings") {
                    auto record_settings_page = std::make_unique<SettingsPage>(SettingsPage::Type::RECORD, &gsr_info, config, &page_stack, supports_window_title);
                    record_settings_page->on_config_changed = [this]() {
                        if(recording_status == RecordingStatus::RECORD)
                            show_notification(TR("Recording settings have been modified. You may need to restart recording to apply the changes."), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD);

                        update_led_indicator_after_settings_change();
                    };
                    page_stack.push(std::move(record_settings_page));
                } else if(id == "pause") {
                    toggle_pause();
                } else if(id == "start") {
                    on_press_start_record(false, RecordForceType::NONE);
                }
            };
            button->set_item_enabled("pause", false);
            main_buttons_list->add_widget(std::move(button));
        }
        {
            auto button = std::make_unique<DropdownButton>(get_theme().title_font_desc.c_str(), get_theme().body_font_desc.c_str(), TR("Livestream"), TR("Not streaming"), &get_theme().stream_button_texture,
                mgl::vec2f(button_width, button_height));
            stream_dropdown_button_ptr = button.get();
            button->add_item(TR("Start"), "start", config.streaming_config.start_stop_hotkey.to_string(false, false));
            button->add_item(TR("Settings"), "settings");
            button->set_item_icon("start", &get_theme().play_texture);
            button->set_item_icon("settings", &get_theme().settings_extra_small_texture);
            button->on_click = [this](const std::string &id) {
                if(id == "settings") {
                    auto stream_settings_page = std::make_unique<SettingsPage>(SettingsPage::Type::STREAM, &gsr_info, config, &page_stack, supports_window_title);
                    stream_settings_page->on_config_changed = [this]() {
                        if(recording_status == RecordingStatus::STREAM)
                            show_notification(TR("Streaming settings have been modified. You may need to restart streaming to apply the changes."), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::STREAM);

                        update_led_indicator_after_settings_change();
                    };
                    page_stack.push(std::move(stream_settings_page));
                } else if(id == "start") {
                    on_press_start_stream(false);
                }
            };
            main_buttons_list->add_widget(std::move(button));
        }

        const mgl::vec2f main_buttons_list_size = main_buttons_list->get_size();
        main_buttons_list->set_position((mgl::vec2f(window_size.x * 0.5f, window_size.y * 0.25f) - main_buttons_list_size * 0.5f).floor());
        front_page_ptr->add_widget(std::move(main_buttons_list));

        {
            const mgl::vec2f main_buttons_size = main_buttons_list_ptr->get_size();
            const int settings_button_size = main_buttons_size.y * 0.33f;
            auto button = std::make_unique<Button>(get_theme().title_font_desc.c_str(), "", mgl::vec2f(settings_button_size, settings_button_size), mgl::Color(0, 0, 0, 180));
            button->set_position((main_buttons_list_ptr->get_position() + main_buttons_size - mgl::vec2f(0.0f, settings_button_size) + mgl::vec2f(settings_button_size * 0.333f, 0.0f)).floor());
            button->set_bg_hover_color(mgl::Color(0, 0, 0, 255));
            button->set_icon(&get_theme().settings_small_texture);
            button->on_click = [this]() {
                open_settings_page();
            };
            front_page_ptr->add_widget(std::move(button));
        }

        {
            const mgl::vec2f main_buttons_size = main_buttons_list_ptr->get_size();
            const int settings_button_size = main_buttons_size.y * 0.33f;
            auto button = std::make_unique<Button>(get_theme().title_font_desc.c_str(), "", mgl::vec2f(settings_button_size, settings_button_size), mgl::Color(0, 0, 0, 180));
            button->set_position((main_buttons_list_ptr->get_position() + main_buttons_size - mgl::vec2f(0.0f, settings_button_size*2) + mgl::vec2f(settings_button_size * 0.333f, 0.0f)).floor());
            button->set_bg_hover_color(mgl::Color(0, 0, 0, 255));
            button->set_icon(&get_theme().screenshot_texture);
            button->set_icon_padding_scale(1.2f);
            button->on_click = [&]() {
                const bool properly_supports_clipboard_image = gsr_info.system_info.display_server == DisplayServer::X11 || wayland_native_clipboard;
                auto screenshot_settings_page = std::make_unique<ScreenshotSettingsPage>(&gsr_info, config, &page_stack, supports_window_title, properly_supports_clipboard_image);
                screenshot_settings_page->on_config_changed = [this]() {
                    update_led_indicator_after_settings_change();
                };
                page_stack.push(std::move(screenshot_settings_page));
            };
            front_page_ptr->add_widget(std::move(button));
        }

        close_button_widget.draw_handler = [&](mgl::Window &window, mgl::vec2f pos, mgl::vec2f size) {
            const int border_size = std::max(1.0f, 0.0015f * get_theme().window_height);
            const float padding_size = std::max(1.0f, 0.003f * get_theme().window_height);
            const mgl::vec2f padding(padding_size, padding_size);
            if(mgl::FloatRect(pos, size).contains(window.get_mouse_position().to_vec2f()))
                draw_rectangle_outline(window, pos.floor(), size.floor(), get_color_theme().tint_color, border_size);

            mgl::Sprite close_sprite(&get_theme().close_texture);
            close_sprite.set_position(pos + padding);
            close_sprite.set_size(size - padding * 2.0f);
            window.draw(close_sprite);
        };

        close_button_widget.event_handler = [&](mgl::Event &event, mgl::Window&, mgl::vec2f pos, mgl::vec2f size) {
            if(event.type == mgl::Event::MouseButtonPressed && event.mouse_button.button == mgl::Mouse::Left) {
                close_button_pressed_inside = mgl::FloatRect(pos, size).contains(mgl::vec2f(event.mouse_button.x, event.mouse_button.y));
            } else if(event.type == mgl::Event::MouseButtonReleased && event.mouse_button.button == mgl::Mouse::Left && close_button_pressed_inside) {
                if(mgl::FloatRect(pos, size).contains(mgl::vec2f(event.mouse_button.x, event.mouse_button.y))) {
                    while(!page_stack.empty()) {
                        page_stack.pop();
                    }
                    return false;
                }
            }
            return true;
        };

        if(gpu_screen_recorder_process > 0) {
            switch(recording_status) {
                case RecordingStatus::NONE:
                    break;
                case RecordingStatus::REPLAY:
                    update_ui_replay_started();
                    break;
                case RecordingStatus::RECORD:
                    update_ui_recording_started();
                    break;
                case RecordingStatus::STREAM:
                    update_ui_streaming_started();
                    break;
            }
        }

        if(paused)
            update_ui_recording_paused();

        if(replay_recording)
            update_ui_recording_started();
    }

    void Overlay::open_settings_page(int scroll_y) {
        auto settings_page = std::make_unique<GlobalSettingsPage>(this, &gsr_info, config, &page_stack);
        settings_page->set_scroll_y(scroll_y);

        settings_page->on_startup_changed = [this](bool enable, int exit_status) {
            if(exit_status == 0)
                return;

            if(exit_status == 67) {
                const bool is_flatpak = getenv("FLATPAK_ID") != nullptr;
                const char *startup_command = is_flatpak ? "flatpak run com.dec05eba.gpu_screen_recorder gsr-ui" : "gsr-ui launch-daemon";
                show_notification(
                    TRF("To enable autorun: install and configure 'dex' (recommended), or manually add '%s' to your desktop autostart entries.", startup_command).c_str(),
                    10.0,
                    mgl::Color(255, 255, 255),
                    mgl::Color(255, 0, 0),
                    NotificationType::NOTICE,
                    nullptr,
                    NotificationLevel::ERROR
                );
                return;
            }

            if(enable)
                show_notification(TR("Failed to add GPU Screen Recorder to system startup"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
            else
                show_notification(TR("Failed to remove GPU Screen Recorder from system startup"), notification_timeout_seconds, mgl::Color(255, 255, 255), mgl::Color(255, 0, 0), NotificationType::NOTICE, nullptr, NotificationLevel::ERROR);
        };

        settings_page->on_click_exit_program_button = [this](std::string_view reason) {
            do_exit = true;
            exit_reason = reason;
        };

        settings_page->on_keyboard_hotkey_changed = [this](std::string_view hotkey_option) {
            recreate_global_hotkeys(hotkey_option);
        };

        settings_page->on_joystick_hotkey_changed = [this](std::string_view hotkey_option) {
#ifndef _WIN32
            global_hotkeys_js.reset();
            if(hotkey_option == "enable_hotkeys")
                global_hotkeys_js = register_joystick_hotkeys(this);
            else if(hotkey_option == "disable_hotkeys")
                global_hotkeys_js.reset();
#else
            /* Joystick hotkeys are Linux-only (GlobalHotkeysJoystick). */
            (void)hotkey_option;
#endif
        };

        settings_page->on_page_closed = [this]() {
            replay_dropdown_button_ptr->set_item_description("start", config.replay_config.start_stop_hotkey.to_string(false, false));
            replay_dropdown_button_ptr->set_item_description("save", config.replay_config.save_hotkey.to_string(false, false));
            replay_dropdown_button_ptr->set_item_description("save_1_min", config.replay_config.save_1_min_hotkey.to_string(false, false));
            replay_dropdown_button_ptr->set_item_description("save_10_min", config.replay_config.save_10_min_hotkey.to_string(false, false));

            record_dropdown_button_ptr->set_item_description("start", config.record_config.start_stop_hotkey.to_string(false, false));
            record_dropdown_button_ptr->set_item_description("pause", config.record_config.pause_unpause_hotkey.to_string(false, false));

            stream_dropdown_button_ptr->set_item_description("start", config.streaming_config.start_stop_hotkey.to_string(false, false));
        };

        settings_page->on_language_changed = [this](int scroll_y) {
            pending_settings_scroll_y = scroll_y;
            reload_ui = true;
            reopen_settings_after_reload = true;
        };

        page_stack.push(std::move(settings_page));
    }

    void Overlay::hide() {
        if(!visible)
            return;

        hide_ui = false;
        reload_ui = false;
        reopen_settings_after_reload = false;
        pending_settings_scroll_y = 0;

        Display *display = x11_dpy;

        while(!page_stack.empty()) {
            page_stack.pop();
        }
        remove_widgets_to_be_removed();

#ifndef _WIN32
        if(default_cursor) {
            XFreeCursor(display, default_cursor);
            default_cursor = 0;
        }

        XUngrabKeyboard(display, CurrentTime);
        XUngrabPointer(display, CurrentTime);
        XFlush(display);

        if(xi_display) {
            cursor_texture.clear();
            cursor_sprite.set_texture(nullptr);
        }
#endif /* !_WIN32 */

#ifndef _WIN32
        window_texture_deinit(&window_texture);
#endif
        window_texture_sprite.set_texture(nullptr);
        screenshot_texture.clear();
        screenshot_sprite.set_texture(nullptr);

        visible = false;
        drawn_first_frame = false;
        start_region_capture = false;
        start_window_capture = false;

        if(xi_input_xev) {
            free(xi_input_xev);
            xi_input_xev = nullptr;
        }

        if(xi_output_xev) {
            free(xi_output_xev);
            xi_output_xev = nullptr;
        }

#ifndef _WIN32
        if(xi_display) {
            if(window) {
                Display *display = x11_dpy;

                const mgl::vec2i new_cursor_position = mgl::vec2i(window->internal_window()->pos.x, window->internal_window()->pos.y) + window->get_mouse_position();
                XWarpPointer(display, DefaultRootWindow(display), DefaultRootWindow(display), 0, 0, 0, 0, new_cursor_position.x, new_cursor_position.y);
                xi_warp_all_mouse_devices(xi_display, new_cursor_position);
                XFlush(display);

                XFixesShowCursor(display, DefaultRootWindow(display));
                XFlush(display);
            }

            XCloseDisplay(xi_display);
            xi_display = nullptr;
        }
#endif /* !_WIN32 */

        if(window) {
            if(show_overlay_timeout_seconds > 0.0001) {
                window->clear(mgl::Color(0, 0, 0, 0));
                window->display();

                mgl_context *context = mgl_get_context();
                context->gl.glFlush();
                context->gl.glFinish();
                usleep(50 * 1000); // EGL doesn't do an immediate flush for some reason
            }

            window->set_visible(false);
            window.reset();
        }

        deinit_theme();
#ifdef __GLIBC__
        malloc_trim(0);
#endif
    }

    void Overlay::hide_next_frame() {
        hide_ui = true;
    }

    void Overlay::toggle_show() {
        if(visible) {
            //hide();
            // We dont want to hide immediately because hide is called in mgl event callback, in which it destroys the mgl window.
            // Instead remove all pages and wait until next iteration to close the UI (which happens when there are no pages to render).
            while(!page_stack.empty()) {
                page_stack.pop();
            }
        } else {
            show();
        }
    }

    void Overlay::toggle_record(RecordForceType force_type) {
        on_press_start_record(false, force_type);
    }

    void Overlay::toggle_pause() {
        if(recording_status != RecordingStatus::RECORD || gpu_screen_recorder_process <= 0)
            return;

#ifdef _WIN32
        send_gsr_control_command(GSR_RECORD_IPC_PIPE, "toggle-pause");
#else
        kill(gpu_screen_recorder_process, SIGUSR2);
#endif
        paused = !paused;

        if(paused) {
            paused_clock.restart();
            update_ui_recording_paused();
            if(config.record_config.record_options.show_notifications)
                show_notification(TR("Recording has been paused"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD);
        } else {
            paused_total_time_seconds += paused_clock.get_elapsed_time_seconds();
            update_ui_recording_unpaused();
            if(config.record_config.record_options.show_notifications)
                show_notification(TR("Recording has been unpaused"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD);
        }

        if(led_indicator && config.record_config.record_options.use_led_indicator)
            led_indicator->blink();
    }

    void Overlay::update_upause_status() {
        paused = false;
        paused_clock.restart();
        paused_total_time_seconds = 0.0;
    }

    void Overlay::toggle_stream() {
        on_press_start_stream(false);
    }

    void Overlay::toggle_replay() {
        on_press_start_replay(false, false, true);
    }

    void Overlay::save_replay() {
        on_press_save_replay();
    }

    void Overlay::save_replay_1_min() {
        on_press_save_replay_1_min_replay();
    }

    void Overlay::save_replay_10_min() {
        on_press_save_replay_10_min_replay();
    }

    void Overlay::take_screenshot() {
        on_press_take_screenshot(false, ScreenshotForceType::NONE);
    }

    void Overlay::take_screenshot_region() {
        on_press_take_screenshot(false, ScreenshotForceType::REGION);
    }

    void Overlay::take_screenshot_window() {
        on_press_take_screenshot(false, ScreenshotForceType::WINDOW);
    }

    const char* Overlay::notification_type_to_string(NotificationType notification_type) {
        switch(notification_type) {
            case NotificationType::NONE:       return nullptr;
            case NotificationType::RECORD:     return "record";
            case NotificationType::REPLAY:     return "replay";
            case NotificationType::STREAM:     return "stream";
            case NotificationType::SCREENSHOT: return "screenshot";
            case NotificationType::NOTICE:     return gsr_icon_path.c_str();
        }
        return nullptr;
    }

    static void truncate_string(std::string &str, int max_length) {
        int index = 0;
        size_t byte_index = 0;

        while(index < max_length && byte_index < str.size()) {
            uint32_t codepoint = 0;
            size_t codepoint_length = 0;
            mgl::utf8_decode((const unsigned char*)str.c_str() + byte_index, str.size() - byte_index, &codepoint, &codepoint_length);
            if(codepoint_length == 0)
                codepoint_length = 1;

            index += 1;
            byte_index += codepoint_length;
        }

        if(byte_index < str.size()) {
            str.erase(byte_index);
            str += "...";
        }
    }

    static bool is_hex_num(char c) {
        return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
    }

    static bool contains_non_hex_number(const char *str) {
        bool hex_start = false;
        size_t len = strlen(str);
        if(len >= 2 && memcmp(str, "0x", 2) == 0) {
            str += 2;
            len -= 2;
            hex_start = true;
        }

        bool is_hex = false;
        for(size_t i = 0; i < len; ++i) {
            char c = str[i];
            if(c == '\0')
                return false;
            if(!is_hex_num(c))
                return true;
            if((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
                is_hex = true;
        }

        return is_hex && !hex_start;
    }

    static bool is_number(const char *str) {
        const char *p = str;
        while(*p) {
            char c = *p;
            if(c < '0' || c > '9')
                return false;
            ++p;
        }
        return true;
    }

    static bool is_capture_target_monitor(const char *capture_target) {
        return strcmp(capture_target, "window") != 0 && strcmp(capture_target, "focused") != 0 && strcmp(capture_target, "region") != 0 && strcmp(capture_target, "portal") != 0 && contains_non_hex_number(capture_target);
    }

    static std::string capture_target_get_notification_name(Display *x11_dpy, const char *capture_target, bool save) {
        std::string result;
        if(is_capture_target_monitor(capture_target)) {
            result = TR("this monitor");
        } else if(is_number(capture_target)) {
            int64_t window_id = 0;
            sscanf(capture_target, "%" PRIi64, &window_id);

            const std::optional<std::string> window_title = x11_dpy ? get_window_title(x11_dpy, window_id) : std::optional<std::string>();
            if(save) {
                result = TR("window");
            } else if(window_title) {
                result = strip(window_title.value());
                truncate_string(result, 30);
                result = TRF("window \"%s\"", result.c_str());
            } else {
                result = TRF("window %s", capture_target);
            }
        } else {
            result = TR(capture_target);
        }
        return result;
    }

    static std::string get_valid_monitor_x11(const std::string &target_monitor_name, const std::vector<Monitor> &monitors) {
        std::string target_monitor_name_clean = target_monitor_name;
        if(starts_with(target_monitor_name_clean, "HDMI-A"))
            target_monitor_name_clean.replace(0, 6, "HDMI");

        for(const Monitor &monitor : monitors) {
            std::string monitor_name_clean = monitor.name;
            if(starts_with(monitor_name_clean, "HDMI-A"))
                monitor_name_clean.replace(0, 6, "HDMI");

            if(target_monitor_name_clean == monitor_name_clean)
                return monitor.name;
        }

        return "";
    }

    static std::string get_focused_monitor_by_cursor(Display *x11_dpy, CursorTracker *cursor_tracker, const GsrInfo &gsr_info, const std::vector<Monitor> &x11_monitors) {
        std::optional<CursorInfo> cursor_info;
        if(cursor_tracker) {
            cursor_tracker->update();
            cursor_info = cursor_tracker->get_latest_cursor_info();
        }

        std::string focused_monitor_name;
        if(cursor_info) {
            focused_monitor_name = std::move(cursor_info->monitor_name);
        } else if(x11_dpy) {
            Window x11_cursor_window = 0;
            mgl::vec2i cursor_position = get_cursor_position(x11_dpy, &x11_cursor_window);

            const mgl::vec2i monitor_position_query_value = (x11_cursor_window || gsr_info.system_info.display_server != DisplayServer::WAYLAND) ? cursor_position : create_window_get_center_position(x11_dpy);
            const Monitor *focused_monitor = find_monitor_at_position(x11_monitors, monitor_position_query_value);
            if(focused_monitor)
                focused_monitor_name = focused_monitor->name;
        }

        return focused_monitor_name;
    }

    void Overlay::show_notification(const char *str, double timeout_seconds, mgl::Color icon_color, mgl::Color bg_color, NotificationType notification_type, const char *capture_target, NotificationLevel notification_level) {
        if(notification_level != NotificationLevel::ERROR)
            timeout_seconds *= notification_duration_multiplier;

        char timeout_seconds_str[32];
        snprintf(timeout_seconds_str, sizeof(timeout_seconds_str), "%f", timeout_seconds);

        const std::string icon_color_str = color_to_hex_str(icon_color);
        const std::string bg_color_str = color_to_hex_str(bg_color);
        const char *notification_args[14] = {
            "gsr-notify", "--text", str, "--timeout", timeout_seconds_str,
            "--icon-color", icon_color_str.c_str(), "--bg-color", bg_color_str.c_str(),
        };

        int arg_index = 9;
        const char *notification_type_str = notification_type_to_string(notification_type);
        if(notification_type_str) {
            notification_args[arg_index++] = "--icon";
            notification_args[arg_index++] = notification_type_str;
        }

        Display *display = x11_dpy;

        std::string monitor_name;
        const auto monitors = get_monitors(display);

        if(capture_target && is_capture_target_monitor(capture_target))
            monitor_name = capture_target;
        else
            monitor_name = get_focused_monitor_by_cursor(x11_dpy, cursor_tracker.get(), gsr_info, monitors);

        monitor_name = get_valid_monitor_x11(monitor_name, monitors);
        if(!monitor_name.empty()) {
            notification_args[arg_index++] = "--monitor";
            notification_args[arg_index++] = monitor_name.c_str();
        } else if(!monitors.empty()) {
            notification_args[arg_index++] = "--monitor";
            notification_args[arg_index++] = monitors.front().name.c_str();
        }

        notification_args[arg_index++] = nullptr;

        if(notification_process > 0) {
            kill(notification_process, SIGINT);
            int status = 0;
            waitpid(notification_process, &status, 0);
        }

        notification_process = exec_program(notification_args, NULL);
    }

    bool Overlay::is_open() const {
        return visible;
    }

    bool Overlay::should_exit(std::string &reason) const {
        reason.clear();
        if(do_exit)
            reason = exit_reason;
        return do_exit;
    }

    void Overlay::exit() {
        do_exit = true;
    }

    void Overlay::go_back_to_old_ui() {
        const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
        if(inside_flatpak)
            exit_reason = "back-to-old-ui";
        else
            exit_reason = "exit";
        exit();
    }

    void Overlay::cancel_region_selection() {
        if(region_selector)
            region_selector->cancel();
    }

    const Config& Overlay::get_config() const {
        return config;
    }

    void Overlay::unbind_all_keyboard_hotkeys() {
        if(global_hotkeys)
            global_hotkeys->unbind_all_keys();
    }

    void Overlay::rebind_all_keyboard_hotkeys() {
        unbind_all_keyboard_hotkeys();
        if(global_hotkeys) {
#ifndef _WIN32
            // TODO: Check if type is GlobalHotkeysLinux
            bind_linux_hotkeys(static_cast<GlobalHotkeysLinux*>(global_hotkeys.get()), this, on_region_selected != nullptr);
#else
            /* bind_linux_hotkeys only uses the base GlobalHotkeys API. */
            bind_linux_hotkeys(global_hotkeys.get(), this, on_region_selected != nullptr);
#endif
        }
    }

    void Overlay::set_notification_speed(NotificationSpeed notification_speed) {
        switch(notification_speed) {
            case NotificationSpeed::NORMAL:
                notification_duration_multiplier = 1.0;
                break;
            case NotificationSpeed::FAST:
                notification_duration_multiplier = 0.3;
                break;
        }
    }

    void Overlay::update_notification_process_status() {
        if(notification_process <= 0)
            return;

        int status;
        if(waitpid(notification_process, &status, WNOHANG) == 0) {
            // Still running
            return;
        }

        notification_process = -1;
    }

    static void string_replace_characters(char *str, const char *characters_to_replace, char new_character) {
        for(; *str != '\0'; ++str) {
            for(const char *p = characters_to_replace; *p != '\0'; ++p) {
                if(*str == *p)
                    *str = new_character;
            }
        }
    }

    static std::string filepath_get_directory(const char *filepath) {
        std::string result = filepath;
        const size_t last_slash_index = result.rfind('/');
        if(last_slash_index == std::string::npos)
            result = ".";
        else
            result.erase(last_slash_index);
        return result;
    }

    static std::string filepath_get_filename(const char *filepath) {
        std::string result = filepath;
        /* Windows paths use backslashes; the engine prints the full path. */
        const size_t last_slash_index = result.rfind('/');
        const size_t last_backslash_index = result.rfind('\\');
        const size_t last_separator_index = std::max(last_slash_index, last_backslash_index);
        if(last_separator_index != std::string::npos)
            result.erase(0, last_separator_index + 1);
        return result;
    }

    static std::string to_duration_string(double duration_sec) {
        int seconds = ceil(duration_sec);

        const int hours = seconds / 60 / 60;
        seconds -= (hours * 60 * 60);

        const int minutes = seconds / 60;
        seconds -= (minutes * 60);

        std::string result;
        if(hours > 0) {
            if (Translation::instance().plural_numbers_are_complex()) {
                result += TRPF("%d hour", hours, hours);
            }
            else {
                if(hours == 1)
                    result += TRF("%d hour", hours);
                else
                    result += TRF("%d hours", hours);
            }
        }

        if(minutes > 0) {
            if(!result.empty())
                result += " ";

            if (Translation::instance().plural_numbers_are_complex()) {
                result += TRPF("%d minute", minutes, minutes);
            }
            else {
                if(minutes == 1)
                    result += TRF("%d minute", minutes);
                else
                    result += TRF("%d minutes", minutes);
            }
        }

        if(seconds > 0 || (hours == 0 && minutes == 0)) {
            if(!result.empty())
                result += " ";

            if (Translation::instance().plural_numbers_are_complex()) {
                result += TRPF("%d second", seconds, seconds);
            }
            else {
                if(seconds == 1)
                    result += TRF("%d second", seconds);
                else
                    result += TRF("%d seconds", seconds);
            }
        }

        return result;
    }

    double Overlay::get_time_passed_in_replay_buffer_seconds() {
        double replay_duration_sec = replay_saved_duration_sec;
        if(replay_duration_sec > current_recording_config.replay_config.replay_time)
            replay_duration_sec = current_recording_config.replay_config.replay_time;
        if(replay_save_duration_min > 0 && replay_duration_sec > replay_save_duration_min * 60)
            replay_duration_sec = replay_save_duration_min * 60;
        return replay_duration_sec;
    }

    static Clipboard::FileType filename_to_clipboard_file_type(const std::string &filename) {
        if(ends_with(filename, ".jpg") || ends_with(filename, ".jpeg"))
            return Clipboard::FileType::JPG;
        else if(ends_with(filename, ".png"))
            return Clipboard::FileType::PNG;
        assert(false);
        return Clipboard::FileType::PNG;
    }

    void Overlay::save_video_in_current_game_directory(std::string &video_filepath, NotificationType notification_type) {
        const std::string video_filename = filepath_get_filename(video_filepath.c_str());

        std::string focused_window_name = desktop_environment->get_focused_window_title();
        if(focused_window_name.empty())
            focused_window_name = "Game";

        focused_window_name = window_title_utf8_sanitize(focused_window_name.c_str(), focused_window_name.size());
        focused_window_name = strip(focused_window_name);
        string_replace_characters(focused_window_name.data(), "/\\", ' ');

        std::string video_directory = filepath_get_directory(video_filepath.c_str()) + "/" + focused_window_name;
        create_directory_recursive(video_directory.data());

        const std::string new_video_filepath = video_directory + "/" + video_filename;
        rename(video_filepath.c_str(), new_video_filepath.c_str());
        video_filepath = new_video_filepath;

        truncate_string(focused_window_name, 40);
        const char *capture_target = nullptr;
        char msg[512];

        switch(notification_type) {
            case NotificationType::RECORD: {
                if(!config.record_config.record_options.show_notifications)
                    return;

                const std::string duration_str = to_duration_string(recording_duration_clock.get_elapsed_time_seconds() - paused_total_time_seconds - (paused ? paused_clock.get_elapsed_time_seconds() : 0.0));
                snprintf(msg, sizeof(msg), TR("Saved a %s recording of %s to \"%s\""),
                    duration_str.c_str(),
                    capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str(), focused_window_name.c_str());
                capture_target = recording_capture_target.c_str();
                break;
            }
            case NotificationType::REPLAY: {
                if(!config.replay_config.record_options.show_notifications)
                    return;

                const std::string duration_str = to_duration_string(get_time_passed_in_replay_buffer_seconds());
                snprintf(msg, sizeof(msg), TR("Saved a %s replay of %s to \"%s\""),
                    duration_str.c_str(),
                    capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str(), focused_window_name.c_str());
                capture_target = recording_capture_target.c_str();
                break;
            }
            case NotificationType::SCREENSHOT: {
                if(!config.screenshot_config.show_notifications)
                    return;

                snprintf(msg, sizeof(msg), TR("Saved a screenshot of %s to \"%s\""),
                    capture_target_get_notification_name(x11_dpy, screenshot_capture_target.c_str(), true).c_str(), focused_window_name.c_str());
                capture_target = screenshot_capture_target.c_str();
                break;
            }
            case NotificationType::NONE:
            case NotificationType::STREAM:
            case NotificationType::NOTICE:
                break;
        }
        show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, notification_type, capture_target);
    }

    void Overlay::on_replay_saved(const char *replay_saved_filepath) {
        replay_save_show_notification = false;

        if(config.replay_config.save_video_in_game_folder) {
            std::string filepath = replay_saved_filepath;
            save_video_in_current_game_directory(filepath, NotificationType::REPLAY);
        } else if(config.replay_config.record_options.show_notifications) {
            const std::string duration_str = to_duration_string(get_time_passed_in_replay_buffer_seconds());

            char msg[512];
            snprintf(msg, sizeof(msg), TR("Saved a %s replay of %s"),
                duration_str.c_str(),
                capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str());
            show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY, recording_capture_target.c_str());
        }

        if(led_indicator && config.replay_config.record_options.use_led_indicator)
            led_indicator->blink();
    }

    void Overlay::process_gsr_game_tracker_output() {
        char buffer[1024];
        if(gsr_game_tracker_process_output_file) {
            char *line = fgets(buffer, sizeof(buffer), gsr_game_tracker_process_output_file);
            if(!line || line[0] == '\0')
                return;

            if(strncmp(line, "Game launched", 13) == 0) {
                game_replay_action = GameReplayAction::START;
            } else if(strncmp(line, "Game exited", 11) == 0) {
                game_replay_action = GameReplayAction::STOP;
            }

        } else if(gsr_game_tracker_process_output_fd > 0) {
            read(gsr_game_tracker_process_output_fd, buffer, sizeof(buffer));
        }
    }

    void Overlay::process_gsr_output() {
        if(replay_save_show_notification && replay_save_clock.get_elapsed_time_seconds() >= replay_saving_notification_timeout_seconds) {
            replay_save_show_notification = false;
            if(config.replay_config.record_options.show_notifications)
                show_notification(TR("Saving replay, this might take some time"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);
        }

        #ifdef _WIN32
        /* Windows: |gpu_screen_recorder_process_output_fd| is the child's
           stdout pipe HANDLE (no POSIX read/write). Peek before reading so
           the UI loop never blocks, then drain available bytes into a
           persistent line buffer and process complete lines. */
        if(gpu_screen_recorder_process_output_fd > 0) {
            const HANDLE handle = (HANDLE)(intptr_t)gpu_screen_recorder_process_output_fd;
            DWORD avail = 0;
            if(!PeekNamedPipe(handle, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                return;

            char chunk[1024];
            DWORD bytes_read = 0;
            if(!ReadFile(handle, chunk, sizeof(chunk) - 1, &bytes_read, nullptr) || bytes_read == 0)
                return;

            gsr_ui_output_buffer.append(chunk, bytes_read);
            size_t newline;
            while((newline = gsr_ui_output_buffer.find('\n')) != std::string::npos) {
                std::string line = gsr_ui_output_buffer.substr(0, newline);
                gsr_ui_output_buffer.erase(0, newline + 1);
                if(line.empty())
                    continue;
                process_gsr_output_line(&line[0]);
            }
        }
#else
        char buffer[1024];
        if(gpu_screen_recorder_process_output_file) {
            char *line = fgets(buffer, sizeof(buffer), gpu_screen_recorder_process_output_file);
            if(!line || line[0] == '\0')
                return;
            process_gsr_output_line(line);
        } else if(gpu_screen_recorder_process_output_fd > 0) {
            read(gpu_screen_recorder_process_output_fd, buffer, sizeof(buffer));
        }
#endif
    }

#ifdef _WIN32
    void Overlay::drain_gsr_process_output() {
        /* Drains the engine's stdout pipe synchronously after the process
           has exited (the update loop is blocked in a waitpid inside the
           stop handler, so process_gsr_output() won't run). Processes the
           same lines as process_gsr_output(). */
        if(gpu_screen_recorder_process_output_fd <= 0)
            return;

        const HANDLE handle = (HANDLE)(intptr_t)gpu_screen_recorder_process_output_fd;
        char chunk[1024];
        while(true) {
            DWORD avail = 0;
            DWORD bytes_read = 0;
            if(!PeekNamedPipe(handle, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
                break;
            if(!ReadFile(handle, chunk, sizeof(chunk) - 1, &bytes_read, nullptr) || bytes_read == 0)
                break;

            gsr_ui_output_buffer.append(chunk, bytes_read);
            size_t newline;
            while((newline = gsr_ui_output_buffer.find('\n')) != std::string::npos) {
                std::string line = gsr_ui_output_buffer.substr(0, newline);
                gsr_ui_output_buffer.erase(0, newline + 1);
                if(line.empty())
                    continue;
                process_gsr_output_line(&line[0]);
            }
        }

        /* Handle any trailing line that arrived without a newline. */
        if(!gsr_ui_output_buffer.empty()) {
            process_gsr_output_line(&gsr_ui_output_buffer[0]);
            gsr_ui_output_buffer.clear();
        }
    }
#endif

    /* Appends one line to %APPDATA%\gpu-screen-recorder\engine.log.
       The engine writes its diagnostics to stderr, which spawn_program()
       folds into the stdout pipe; without persisting them here a GUI build
       with no console loses every clue about why a recording failed. */
    static void append_engine_log(const char *line) {
        static FILE *engine_log_file = nullptr;
        static bool engine_log_opened = false;
        if(!engine_log_opened) {
            engine_log_opened = true;
            std::string log_dir = get_config_dir();
            create_directory_recursive(log_dir.data());
            const std::string log_path = log_dir + "/engine.log";
            engine_log_file = fopen(log_path.c_str(), "a");
            if(!engine_log_file)
                fprintf(stderr, "gsr-ui warning: could not open the engine log at %s\n", log_path.c_str());
        }
        if(engine_log_file) {
            fprintf(engine_log_file, "%s\n", line);
            fflush(engine_log_file);
        }
    }

    void Overlay::process_gsr_output_line(char *line) {
        int line_len = strlen(line);
        if(line_len > 0 && line[line_len - 1] == '\n') {
            line[line_len - 1] = '\0';
            line_len -= 1;
        }
        if(line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
            line_len -= 1;
        }
        if(line_len == 0)
            return;

        append_engine_log(line);

        const std::string_view line_view{line, (size_t)line_len};
        if(starts_with(line_view, "gsr error: ")) {
            //show_notification(line + 11, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), recording_status_to_notification_type(recording_status), nullptr, NotificationLevel::ERROR);
            if(replay_recording && ends_with(line_view, "recording")) {
                std::string dummy;
                on_stop_recording(1, dummy);
            } else if(recording_status == RecordingStatus::REPLAY && ends_with(line_view, "replay")) {
                show_notification(TR("Failed to save replay, make sure the replay save directory is mounted and writable"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
                replay_save_show_notification = false;
            }
            return;
        }

        const std::string video_filepath = filepath_get_filename(line);
        if(starts_with(video_filepath, "Video_")) {
            record_filepath = line;
            on_stop_recording(0, record_filepath);
            return;
        }

        switch(recording_status) {
            case RecordingStatus::NONE:
                break;
            case RecordingStatus::REPLAY:
                on_replay_saved(line);
                break;
            case RecordingStatus::RECORD:
                break;
            case RecordingStatus::STREAM:
                break;
        }
    }

    void Overlay::on_gsr_process_error(int exit_code, NotificationType notification_type) {
        fprintf(stderr, "Warning: gpu-screen-recorder (%d) exited with exit status %d\n", (int)gpu_screen_recorder_process, exit_code);
        {
            char exit_line[128];
            snprintf(exit_line, sizeof(exit_line), "gsr-ui: gpu-screen-recorder exited with status %d", exit_code);
            append_engine_log(exit_line);
        }
        if(exit_code == 50) {
            show_notification(TR("Desktop portal capture failed. Either you canceled the desktop portal or your Wayland compositor doesn't support desktop portal capture or it's incorrectly setup on your system."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 51) {
            show_notification(TR("Monitor capture failed. The monitor you are trying to capture is invalid. Please validate your capture settings."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 52) {
            show_notification(TR("Capture failed. Neither H264, HEVC nor AV1 video codecs are supported on your system or you are trying to capture at a resolution higher than your system supports for each video codec."), 10.0, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 53) {
            show_notification(TR("Capture failed. Your system doesn't support the resolution you are trying to record at with the video codec you have chosen. Change capture resolution or video codec and try again. Note: AV1 supports the highest resolution, then HEVC and then H264."), 10.0, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 54) {
            show_notification(TR("Capture failed. Your system doesn't support the video codec you have chosen. Change video codec and try again."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else if(exit_code == 60) {
            show_notification(TR("Stopped capture because the user canceled the desktop portal"), notification_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        } else {
            const char *prefix = "";
            switch(notification_type) {
                case NotificationType::NONE:
                case NotificationType::NOTICE:
                    break;
                case NotificationType::SCREENSHOT:
                    prefix = TR("Failed to take a screenshot");
                    break;
                case NotificationType::RECORD:
                    prefix = TR("Failed to start/save recording");
                    break;
                case NotificationType::REPLAY:
                    prefix = TR("Replay stopped because of an error");
                    break;
                case NotificationType::STREAM:
                    prefix = TR("Streaming stopped because of an error");
                    break;
            }

            char msg[256];
            snprintf(msg, sizeof(msg), TR("%s. Verify if settings are correct"), prefix);
            show_notification(msg, notification_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), notification_type, nullptr, NotificationLevel::ERROR);
        }
    }

    void Overlay::update_gsr_process_status() {
        if(gpu_screen_recorder_process <= 0)
            return;

        int status;
        if(waitpid(gpu_screen_recorder_process, &status, WNOHANG) == 0) {
            // Still running
            return;
        }

        close_gpu_screen_recorder_output();

        int exit_code = -1;
        if(WIFEXITED(status))
            exit_code = WEXITSTATUS(status);

        switch(recording_status) {
            case RecordingStatus::NONE:
                break;
            case RecordingStatus::REPLAY: {
                replay_save_duration_min = 0;
                update_ui_replay_stopped();
                if(exit_code == 0) {
                    if(config.replay_config.record_options.show_notifications)
                        show_notification(TR("Replay stopped"), short_notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);
                } else {
                    on_gsr_process_error(exit_code, NotificationType::REPLAY);
                }

                if(led_indicator)
                    led_indicator->set_led(false);

                break;
            }
            case RecordingStatus::RECORD: {
                update_ui_recording_stopped();
                on_stop_recording(exit_code, record_filepath);

                if(led_indicator)
                    led_indicator->set_led(false);
                break;
            }
            case RecordingStatus::STREAM: {
                update_ui_streaming_stopped();
                if(exit_code == 0) {
                    if(config.streaming_config.record_options.show_notifications)
                        show_notification(TR("Streaming has stopped"), short_notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::STREAM);
                } else {
                    on_gsr_process_error(exit_code, NotificationType::STREAM);
                }

                if(led_indicator)
                    led_indicator->set_led(false);
                break;
            }
        }

        gpu_screen_recorder_process = -1;
        recording_status = RecordingStatus::NONE;
        replay_launched_manually = true;
    }

    void Overlay::update_gsr_screenshot_process_status() {
        if(gpu_screen_recorder_screenshot_process <= 0)
            return;

        int status;
        if(waitpid(gpu_screen_recorder_screenshot_process, &status, WNOHANG) == 0) {
            // Still running
            return;
        }

        int exit_code = -1;
        if(WIFEXITED(status))
            exit_code = WEXITSTATUS(status);

        if(exit_code == 0) {
            if(config.screenshot_config.save_screenshot_in_game_folder && config.screenshot_config.save_screenshot_to_disk) {
                save_video_in_current_game_directory(screenshot_filepath, NotificationType::SCREENSHOT);
            } else if(config.screenshot_config.show_notifications) {
                char msg[512];
                snprintf(msg, sizeof(msg), TR("Saved a screenshot of %s"), capture_target_get_notification_name(x11_dpy, screenshot_capture_target.c_str(), true).c_str());
                show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::SCREENSHOT, screenshot_capture_target.c_str());
            }

            if(config.screenshot_config.save_screenshot_to_clipboard && clipboard)
                clipboard->set_current_file(screenshot_filepath, filename_to_clipboard_file_type(screenshot_filepath));

            if(led_indicator && config.screenshot_config.use_led_indicator)
                led_indicator->blink();

            if(!strip(config.screenshot_config.custom_script).empty()) {
                std::stringstream ss;
                ss << config.screenshot_config.custom_script << " " << std::quoted(screenshot_filepath);
                const std::string command = ss.str();
                const char *args[] = { "/bin/sh", "-c", command.c_str(), nullptr };
                exec_program_on_host_daemonized(args);
            }
        } else {
            fprintf(stderr, "Warning: gpu-screen-recorder (%d) exited with exit status %d\n", (int)gpu_screen_recorder_screenshot_process, exit_code);
            show_notification(TR("Failed to take a screenshot. Verify if settings are correct"), notification_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::SCREENSHOT, nullptr, NotificationLevel::ERROR);
        }

        gpu_screen_recorder_screenshot_process = -1;
    }

    void Overlay::replay_status_update_status() {
        if(replay_status_update_clock.get_elapsed_time_seconds() < replay_status_update_check_timeout_seconds)
            return;

        replay_status_update_clock.restart();
        update_power_supply_status();
        update_replay_program_startup_status();
    }

    // TODO: Instead of checking power supply status periodically listen to power supply event
    void Overlay::update_power_supply_status() {
        if(replay_startup_mode == ReplayStartupMode::DONT_TURN_ON_AUTOMATICALLY)
            return;

        power_supply_connected = power_supply_online_filepath.empty() || power_supply_is_connected(power_supply_online_filepath.c_str());
    }

    bool Overlay::replay_program_autostart_capture_options_available() {
        return are_all_audio_tracks_available_to_capture(config.replay_config.record_options.audio_tracks_list) && is_webcam_available_to_capture(config.replay_config.record_options);
    }

    void Overlay::update_replay_program_startup_status() {
        if(replay_startup_mode != ReplayStartupMode::TURN_ON_AT_SYSTEM_STARTUP)
            return;

        if(config.replay_config.only_start_replay_if_power_supply_connected) {
            if(recording_status == RecordingStatus::NONE) {
                if(replay_program_autostart_capture_options_available() || replay_launched_once) {
                    const bool power_supply_connected_status_changed = power_supply_connected != replay_program_startup_power_supply_connected;
                    replay_program_startup_power_supply_connected = power_supply_connected;

                    if(power_supply_connected_status_changed && power_supply_connected)
                        on_press_start_replay(false, false);
                }
            } else if(recording_status == RecordingStatus::REPLAY) {
                const bool power_supply_connected_status_changed = power_supply_connected != replay_program_startup_power_supply_connected;
                replay_program_startup_power_supply_connected = power_supply_connected;

                if(power_supply_connected_status_changed && !power_supply_connected && !replay_launched_manually)
                    on_press_start_replay(false, false);
            }
        } else {
            if(recording_status == RecordingStatus::NONE && !replay_launched_once) {
                if(replay_program_autostart_capture_options_available())
                    on_press_start_replay(false, false);
            }
        }
    }

    void Overlay::update_gsr_game_tracker_replay_status() {
        if(replay_startup_mode != ReplayStartupMode::TURN_ON_AT_GAME_LAUNCH)
            return;

        const bool power_supply_allows_start = !config.replay_config.only_start_replay_if_power_supply_connected || power_supply_connected;
        const bool power_supply_disconnected = config.replay_config.only_start_replay_if_power_supply_connected && !power_supply_connected;

        if(replay_launched_manually)
            game_replay_action = GameReplayAction::IDLE;

        if(recording_status == RecordingStatus::NONE && game_replay_action == GameReplayAction::START && power_supply_allows_start) {
            on_press_start_replay(false, false);
        } else if(recording_status == RecordingStatus::REPLAY && (game_replay_action == GameReplayAction::STOP || power_supply_disconnected)) {
            on_press_start_replay(false, false);
        }

        game_replay_action = GameReplayAction::IDLE;
    }

    void Overlay::on_stop_recording(int exit_code, std::string &video_filepath) {
        if(recording_status != RecordingStatus::RECORD && recording_status != RecordingStatus::REPLAY)
            return;

        if(exit_code == 0) {
            if(config.record_config.save_video_in_game_folder) {
                save_video_in_current_game_directory(video_filepath, NotificationType::RECORD);
            } else if(config.record_config.record_options.show_notifications) {
                const std::string duration_str = to_duration_string(recording_duration_clock.get_elapsed_time_seconds() - paused_total_time_seconds - (paused ? paused_clock.get_elapsed_time_seconds() : 0.0));

                char msg[512];
                snprintf(msg, sizeof(msg), TR("Saved a %s recording of %s"),
                    duration_str.c_str(),
                    capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), true).c_str());
                show_notification(msg, notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::RECORD, recording_capture_target.c_str());
            }

            if(led_indicator) {
                if(recording_status == RecordingStatus::REPLAY && !current_recording_config.replay_config.record_options.use_led_indicator)
                    led_indicator->set_led(false);
                else if(recording_status == RecordingStatus::STREAM && !current_recording_config.streaming_config.record_options.use_led_indicator)
                    led_indicator->set_led(false);
                else if(config.record_config.record_options.use_led_indicator)
                    led_indicator->blink();
            }
        } else {
            on_gsr_process_error(exit_code, NotificationType::RECORD);
        }

        update_ui_recording_stopped();
        replay_recording = false;
    }

    void Overlay::update_ui_recording_paused() {
        if(!visible || recording_status != RecordingStatus::RECORD)
            return;

        record_dropdown_button_ptr->set_description(TR("Paused"));
        record_dropdown_button_ptr->set_item_label("pause", TR("Unpause"));
        record_dropdown_button_ptr->set_item_icon("pause", &get_theme().play_texture);
    }

    void Overlay::update_ui_recording_unpaused() {
        if(!visible || recording_status != RecordingStatus::RECORD)
            return;

        record_dropdown_button_ptr->set_description(TR("Recording"));
        record_dropdown_button_ptr->set_item_label("pause", TR("Pause"));
        record_dropdown_button_ptr->set_item_icon("pause", &get_theme().pause_texture);
    }

    void Overlay::update_ui_recording_started() {
        if(!visible)
            return;

        record_dropdown_button_ptr->set_item_label("start", TR("Stop and save"));
        record_dropdown_button_ptr->set_activated(true);
        record_dropdown_button_ptr->set_description(TR("Recording"));
        record_dropdown_button_ptr->set_item_icon("start", &get_theme().stop_texture);
        record_dropdown_button_ptr->set_item_enabled("pause", recording_status == RecordingStatus::RECORD);
    }

    void Overlay::update_ui_recording_stopped() {
        if(!visible)
            return;

        record_dropdown_button_ptr->set_item_label("start", TR("Start"));
        record_dropdown_button_ptr->set_activated(false);
        record_dropdown_button_ptr->set_description(TR("Not recording"));
        record_dropdown_button_ptr->set_item_icon("start", &get_theme().play_texture);

        record_dropdown_button_ptr->set_item_label("pause", TR("Pause"));
        record_dropdown_button_ptr->set_item_icon("pause", &get_theme().pause_texture);
        record_dropdown_button_ptr->set_item_enabled("pause", false);
        update_upause_status();
        replay_recording = false;
    }

    void Overlay::update_ui_streaming_started() {
        if(!visible)
            return;

        stream_dropdown_button_ptr->set_item_label("start", TR("Stop"));
        stream_dropdown_button_ptr->set_activated(true);
        stream_dropdown_button_ptr->set_description(TR("Streaming"));
        stream_dropdown_button_ptr->set_item_icon("start", &get_theme().stop_texture);
    }

    void Overlay::update_ui_streaming_stopped() {
        if(!visible)
            return;

        stream_dropdown_button_ptr->set_item_label("start", TR("Start"));
        stream_dropdown_button_ptr->set_activated(false);
        stream_dropdown_button_ptr->set_description(TR("Not streaming"));
        stream_dropdown_button_ptr->set_item_icon("start", &get_theme().play_texture);
        update_ui_recording_stopped();
    }

    void Overlay::update_ui_replay_started() {
        if(!visible)
            return;

        replay_dropdown_button_ptr->set_item_label("start", TR("Turn off"));
        replay_dropdown_button_ptr->set_activated(true);
        replay_dropdown_button_ptr->set_description(TR("On"));
        replay_dropdown_button_ptr->set_item_icon("start", &get_theme().stop_texture);
        replay_dropdown_button_ptr->set_item_enabled("save", true);
        replay_dropdown_button_ptr->set_item_enabled("save_1_min", current_recording_config.replay_config.replay_time >= 60);
        replay_dropdown_button_ptr->set_item_enabled("save_10_min", current_recording_config.replay_config.replay_time >= 60 * 10);
    }

    void Overlay::update_ui_replay_stopped() {
        if(!visible)
            return;

        replay_dropdown_button_ptr->set_item_label("start", TR("Turn on"));
        replay_dropdown_button_ptr->set_activated(false);
        replay_dropdown_button_ptr->set_description(TR("Off"));
        replay_dropdown_button_ptr->set_item_icon("start", &get_theme().play_texture);
        replay_dropdown_button_ptr->set_item_enabled("save", false);
        replay_dropdown_button_ptr->set_item_enabled("save_1_min", false);
        replay_dropdown_button_ptr->set_item_enabled("save_10_min", false);
        update_ui_recording_stopped();
    }

    static std::string get_date_str() {
        char str[128];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
        return str;
    }

    static std::string container_to_file_extension(const std::string &container) {
        if(container == "matroska")
            return "mkv";
        else if(container == "mpegts")
            return "ts";
        else if(container == "hls")
            return "m3u8";
        else
            return container;
    }

    static std::vector<std::string> create_audio_tracks_cli_args(const std::vector<AudioTrack> &audio_tracks, const GsrInfo &gsr_info) {
        std::vector<std::string> result;
        result.reserve(audio_tracks.size());

        for(const AudioTrack &audio_track : audio_tracks) {
            bool has_audio = false;
            std::string audio_track_merged;
            int num_app_audio = 0;

            if(!audio_track.name.empty())
                audio_track_merged = "name:" + audio_track.name;

            for(const std::string &audio_input_name : audio_track.audio_inputs) {
                std::string new_audio_input_name = audio_input_name;
                const bool is_app_audio = starts_with(new_audio_input_name, "app:");
                if(is_app_audio && !gsr_info.system_info.supports_app_audio)
                    continue;

                if(is_app_audio && audio_track.application_audio_invert)
                    new_audio_input_name.replace(0, 4, "app-inverse:");

                if(is_app_audio)
                    ++num_app_audio;

                if(!audio_track_merged.empty())
                    audio_track_merged += "|";

                audio_track_merged += new_audio_input_name;
                has_audio = true;
            }

            if(num_app_audio == 0 && audio_track.application_audio_invert) {
                if(!audio_track_merged.empty())
                    audio_track_merged += "|";

                audio_track_merged += "app-inverse:";
                has_audio = true;
            }

            if(has_audio)
                result.push_back(std::move(audio_track_merged));
        }

        return result;
    }

    void Overlay::add_region_command(std::vector<const char*> &args, char *region_str, int region_str_size) {
        Region region = region_selector->get_region_selection(x11_dpy, wayland_dpy);
        if(region.size.x <= 32 && region.size.y <= 32) {
            region.size.x = 0;
            region.size.y = 0;
        }
        snprintf(region_str, region_str_size, "%dx%d+%d+%d", region.size.x, region.size.y, region.pos.x, region.pos.y);
        args.push_back("-region");
        args.push_back(region_str);
    }

    void Overlay::add_common_gpu_screen_recorder_args(
        std::vector<const char*> &args,
        const MainConfig &main_config,
        const RecordOptions &record_options,
        const std::vector<std::string> &audio_tracks,
        const std::string &video_bitrate,
        const char *region,
        char *region_str,
        int region_str_size,
        const std::string &region_area_option,
        RecordForceType force_type)
    {
        if(record_options.video_quality == "custom") {
            args.push_back("-bm");
            args.push_back("cbr");
            args.push_back("-q");
            args.push_back(video_bitrate.c_str());
        } else {
            args.push_back("-q");
            args.push_back(record_options.video_quality.c_str());
        }

        if(region_area_option == "focused" || record_options.change_video_resolution) {
            args.push_back("-s");
            args.push_back(region);
        }

        for(const std::string &audio_track : audio_tracks) {
            args.push_back("-a");
            args.push_back(audio_track.c_str());
        }

        if(record_options.restore_portal_session && force_type != RecordForceType::WINDOW) {
            args.push_back("-restore-portal-session");
            args.push_back("yes");
        }

        if(record_options.low_power_mode) {
            args.push_back("-low-power");
            args.push_back("yes");
        }

        if(main_config.exclude_metadata) {
            args.push_back("-exclude-metadata");
            args.push_back("yes");
        }

        if(region_area_option == "region")
            add_region_command(args, region_str, region_str_size);
    }

    static bool validate_capture_target(const std::string &capture_target, const SupportedCaptureOptions &capture_options) {
        if(capture_target == "window") {
            return capture_options.window;
        } else if(capture_target == "focused") {
            return capture_options.focused;
        } else if(capture_target == "region") {
            return capture_options.region;
        } else if(capture_target == "portal") {
            return capture_options.portal;
        } else if(capture_target == "focused_monitor") {
            return !capture_options.monitors.empty();
        } else {
            for(const GsrMonitor &monitor : capture_options.monitors) {
                if(capture_target == monitor.name)
                    return true;
            }
            return false;
        }
    }

    static std::string get_valid_capture_target(const std::string &capture_target, const SupportedCaptureOptions &capture_options) {
        std::string capture_target_clean = capture_target;
        if(starts_with(capture_target_clean, "HDMI-A"))
            capture_target_clean.replace(0, 6, "HDMI");

        for(const GsrMonitor &monitor : capture_options.monitors) {
            std::string monitor_name_clean = monitor.name;
            if(starts_with(monitor_name_clean, "HDMI-A"))
                monitor_name_clean.replace(0, 6, "HDMI");

            if(capture_target_clean == monitor_name_clean)
                return monitor.name;
        }

        return "";
    }

    std::string Overlay::get_capture_target(const std::string &capture_target, const SupportedCaptureOptions &capture_options) {
        if(capture_target == "window") {
            return std::to_string(region_selector->get_window_selection());
        } else if(capture_target == "focused_monitor") {
            std::optional<CursorInfo> cursor_info;
            if(cursor_tracker) {
                cursor_tracker->update();
                cursor_info = cursor_tracker->get_latest_cursor_info();
            }

            std::string focused_monitor_name;
            if(cursor_info) {
                focused_monitor_name = std::move(cursor_info->monitor_name);
            } else {
                Display *display = x11_dpy;
                focused_monitor_name = get_focused_monitor_by_cursor(x11_dpy, cursor_tracker.get(), gsr_info, get_monitors(display));
            }

            focused_monitor_name = get_valid_capture_target(focused_monitor_name, capture_options);
            if(!focused_monitor_name.empty())
                return focused_monitor_name;
            else if(!capture_options.monitors.empty())
                return capture_options.monitors.front().name;
            else
                return "";
        } else {
            return capture_target;
        }
    }

    void Overlay::prepare_gsr_output_for_reading() {
        if(gpu_screen_recorder_process_output_fd <= 0)
            return;

#ifndef _WIN32
        const int fdl = fcntl(gpu_screen_recorder_process_output_fd, F_GETFL);
        fcntl(gpu_screen_recorder_process_output_fd, F_SETFL, fdl | O_NONBLOCK);
        gpu_screen_recorder_process_output_file = fdopen(gpu_screen_recorder_process_output_fd, "r");
        if(gpu_screen_recorder_process_output_file)
            gpu_screen_recorder_process_output_fd = -1;
#else
        /* On Windows the fd already holds the child's stdout pipe HANDLE,
           which process_gsr_output() reads directly via PeekNamedPipe/
           ReadFile — nothing to do here. */
        (void)0;
#endif
    }

#ifdef _WIN32
    /* Sends a control command to the engine's -ipc named pipe through
       gsr-cli, detached. The engine completes the command asynchronously;
       for deferred commands (save-replay/stop) it prints the result
       filepath to its stdout, which process_gsr_output() picks up. */
    static void send_gsr_control_command(const char *pipe_name, const char *command, const char *arg) {
        if(arg) {
            const char *args[] = { "gsr-cli", "-ipc", pipe_name, command, arg, nullptr };
            gsr::exec_program_daemonized(args, true);
        } else {
            const char *args[] = { "gsr-cli", "-ipc", pipe_name, command, nullptr };
            gsr::exec_program_daemonized(args, true);
        }
    }

    /* Waits up to |timeout_ms| for the engine to finalize and exit on its
       own (the gsr-cli stop command was already sent), then force-terminates
       it as a fallback so a stuck engine can never hang the UI in a blocking
       waitpid. Returns the engine's exit code, or -1 if it had to be
       force-terminated. */
    static int wait_gsr_process_exit(pid_t process, int timeout_ms) {
        if(process <= 0)
            return -1;

        int status = 0;
        const int step_ms = 50;
        for(int i = 0; i < timeout_ms / step_ms; ++i) {
            if(waitpid(process, &status, WNOHANG) != 0)
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            usleep(step_ms * 1000);
        }

        fprintf(stderr, "gsr-ui warning: gpu-screen-recorder (%d) did not exit after %d ms, force-terminating it\n", (int)process, timeout_ms);
        kill(process, SIGTERM);
        /* Bounded: waitpid() also reports "still running" when the handle is
           no longer valid (WaitForSingleObject fails), so an unbounded loop
           here could hang the UI thread forever. Give the terminated process
           a few seconds and then stop waiting regardless. */
        const int force_timeout_ms = 5000;
        for(int i = 0; i < force_timeout_ms / step_ms; ++i) {
            if(waitpid(process, &status, WNOHANG) != 0)
                return -1;
            usleep(step_ms * 1000);
        }
        fprintf(stderr, "gsr-ui warning: gpu-screen-recorder (%d) still did not exit after being force-terminated, giving up\n", (int)process);
        return -1;
    }
#endif

    void Overlay::on_press_save_replay() {
        if(recording_status != RecordingStatus::REPLAY || gpu_screen_recorder_process <= 0)
            return;

        replay_save_duration_min = 0;
        replay_save_show_notification = true;
        replay_save_clock.restart();
        replay_saved_duration_sec = replay_duration_clock.get_elapsed_time_seconds();
        if(replay_restart_on_save)
            replay_duration_clock.restart();

#ifdef _WIN32
        /* Windows: no POSIX signals — the save command goes over the
           engine's -ipc pipe via gsr-cli (detached; the engine prints the
           saved filepath to stdout, which process_gsr_output() surfaces). */
        send_gsr_control_command(GSR_REPLAY_IPC_PIPE, "save-replay");
#else
        kill(gpu_screen_recorder_process, SIGUSR1);
#endif
    }

    void Overlay::on_press_save_replay_1_min_replay() {
        if(recording_status != RecordingStatus::REPLAY || gpu_screen_recorder_process <= 0)
            return;

        if(current_recording_config.replay_config.replay_time < 60)
            return;

        replay_save_duration_min = 1;
        replay_save_show_notification = true;
        replay_save_clock.restart();
        replay_saved_duration_sec = replay_duration_clock.get_elapsed_time_seconds();
#ifdef _WIN32
        send_gsr_control_command(GSR_REPLAY_IPC_PIPE, "save-replay", "60");
#else
        kill(gpu_screen_recorder_process, SIGRTMIN+3);
#endif
    }

    void Overlay::on_press_save_replay_10_min_replay() {
        if(recording_status != RecordingStatus::REPLAY || gpu_screen_recorder_process <= 0)
            return;

        if(current_recording_config.replay_config.replay_time < 60 * 10)
            return;

        replay_save_duration_min = 10;
        replay_save_show_notification = true;
        replay_save_clock.restart();
        replay_saved_duration_sec = replay_duration_clock.get_elapsed_time_seconds();
#ifdef _WIN32
        send_gsr_control_command(GSR_REPLAY_IPC_PIPE, "save-replay", "600");
#else
        kill(gpu_screen_recorder_process, SIGRTMIN+5);
#endif
    }

    static const char* get_first_usable_hardware_video_codec_name(const GsrInfo &gsr_info) {
        if(gsr_info.supported_video_codecs.h264)
            return "h264";
        else if(gsr_info.supported_video_codecs.hevc)
            return "hevc";
        else if(gsr_info.supported_video_codecs.av1)
            return "av1";
        else if(gsr_info.supported_video_codecs.vp8)
            return "vp8";
        else if(gsr_info.supported_video_codecs.vp9)
            return "vp9";
        return nullptr;
    }

    static const char* change_container_if_codec_not_supported(const char *video_codec, const char *container) {
        if(strcmp(video_codec, "vp8") == 0 || strcmp(video_codec, "vp9") == 0) {
            if(strcmp(container, "webm") != 0 && strcmp(container, "matroska") != 0) {
                fprintf(stderr, "Warning: container '%s' is not compatible with video codec '%s', using webm container instead\n", container, video_codec);
                return "webm";
            }
        } else if(strcmp(container, "webm") == 0) {
            fprintf(stderr, "Warning: container webm is not compatible with video codec '%s', using mp4 container instead\n",  video_codec);
            return "mp4";
        }
        return container;
    }

    static void choose_video_codec_and_container_with_fallback(const GsrInfo &gsr_info, const RecordOptions &record_options, const char **video_codec, const char **container, const char **encoder) {
        *encoder = "gpu";
        if(strcmp(*video_codec, "h264_software") == 0) {
            *video_codec = "h264";
            *encoder = "cpu";
        } else if(strcmp(*video_codec, "auto") == 0) {
            if(!get_first_usable_hardware_video_codec_name(gsr_info)) {
                *video_codec = "h264";
                *encoder = "cpu";
            }
        }
        *container = change_container_if_codec_not_supported(*video_codec, *container);

        if(record_options.enable_vulkan_video_encoding && strcmp(*encoder, "gpu") == 0) {
            if(strcmp(*video_codec, "auto") == 0)
                *video_codec = "h264_vulkan";
            else if(strcmp(*video_codec, "h264") == 0)
                *video_codec = "h264_vulkan";
            else if(strcmp(*video_codec, "hevc") == 0)
                *video_codec = "hevc_vulkan";
            else if(strcmp(*video_codec, "hevc_hdr") == 0)
                *video_codec = "hevc_hdr_vulkan";
            else if(strcmp(*video_codec, "hevc_10bit") == 0)
                *video_codec = "hevc_10bit_vulkan";
            else if(strcmp(*video_codec, "av1") == 0)
                *video_codec = "av1_vulkan";
            else if(strcmp(*video_codec, "av1_hdr") == 0)
                *video_codec = "av1_hdr_vulkan";
            else if(strcmp(*video_codec, "av1_10bit") == 0)
                *video_codec = "av1_10bit_vulkan";
        }
    }

    static std::string get_framerate_mode_validate(const RecordOptions &record_options, const GsrInfo &gsr_info) {
        (void)gsr_info;
        std::string framerate_mode = record_options.framerate_mode;
        if(framerate_mode == "auto") {
#ifdef _WIN32
            /* "auto" means VFR upstream, where the X11/Wayland capture is
               damage-driven so the frame timestamps follow the content.
               The Windows backends (WGC and DXGI Desktop Duplication) are
               polled from the recorder loop instead: the capture delivers a
               frame only if one happens to be pending when the loop ticks,
               so VFR timestamps inherit that jitter and frames get dropped.
               Measured on this port (1080p60, DXGI, GTX 950):
                 VFR -> 54.5 fps with 300+ distinct frame intervals
                 CFR -> 60.00 fps with every frame at 16.67 ms
               so "auto" maps to CFR on Windows to keep playback smooth. */
            framerate_mode = "cfr";
#else
            framerate_mode = "vfr";
#endif
        }
        return framerate_mode;
    }

    struct CameraAlignment {
        std::string halign;
        std::string valign;
        mgl::vec2i pos;
    };

    static CameraAlignment position_to_alignment(mgl::vec2i pos, mgl::vec2i size) {
        const mgl::vec2i pos_overflow = mgl::vec2i(100, 100) - (pos + size);
        if(pos_overflow.x < 0)
            pos.x += pos_overflow.x;
        if(pos_overflow.y < 0)
            pos.y += pos_overflow.y;

        if(pos.x < 0)
            pos.x = 0;
        if(pos.y < 0)
            pos.y = 0;

        CameraAlignment camera_alignment;
        const mgl::vec2i center = pos + size/2;

        if(center.x < 50) {
            camera_alignment.halign = "start";
            camera_alignment.pos.x = pos.x;
        } else {
            camera_alignment.halign = "end";
            camera_alignment.pos.x = -(100 - (pos.x + size.x));
        }

        if(center.y < 50) {
            camera_alignment.valign = "start";
            camera_alignment.pos.y = pos.y;
        } else {
            camera_alignment.valign = "end";
            camera_alignment.pos.y = -(100 - (pos.y + size.y));
        }

        return camera_alignment;
    }

    static std::string compose_capture_source_arg(const std::string &capture_target, const RecordOptions &record_options) {
        std::string capture_source_arg = capture_target;
        if(!record_options.webcam_source.empty()) {
            const mgl::vec2i webcam_size(record_options.webcam_width, record_options.webcam_height);
            const CameraAlignment camera_alignment = position_to_alignment(mgl::vec2i(record_options.webcam_x, record_options.webcam_y), webcam_size);

            capture_source_arg += "|" + record_options.webcam_source;
            capture_source_arg += ";halign=" + camera_alignment.halign;
            capture_source_arg += ";valign=" + camera_alignment.valign;
            capture_source_arg += ";x=" + std::to_string(camera_alignment.pos.x) + "%";
            capture_source_arg += ";y=" + std::to_string(camera_alignment.pos.y) + "%";
            capture_source_arg += ";width=" + std::to_string(webcam_size.x) + "%";
            capture_source_arg += ";height=" + std::to_string(webcam_size.y) + "%";
            capture_source_arg += ";pixfmt=" + record_options.webcam_video_format;
            capture_source_arg += ";camera_width=" + std::to_string(record_options.webcam_camera_width);
            capture_source_arg += ";camera_height=" + std::to_string(record_options.webcam_camera_height);
            capture_source_arg += ";camera_fps=" + std::to_string(record_options.webcam_camera_fps);
            if(record_options.webcam_flip_horizontally)
                capture_source_arg += ";hflip=true";
        }
        return capture_source_arg;
    }

    bool Overlay::on_press_start_replay(bool disable_notification, bool finished_selection, bool launched_manually) {
        if(region_selector->is_started())
            return false;

        switch(recording_status) {
            case RecordingStatus::NONE:
            case RecordingStatus::REPLAY:
                break;
            case RecordingStatus::RECORD:
                show_notification(TR("Unable to start replay when recording. Stop recording before starting replay."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
                return false;
            case RecordingStatus::STREAM:
                show_notification(TR("Unable to start replay when streaming. Stop streaming before starting replay."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
                return false;
        }

        update_upause_status();
        replay_launched_manually = launched_manually;
        replay_launched_once = true;

        if(gpu_screen_recorder_process > 0) {
#ifdef _WIN32
            /* Windows: graceful stop over the -ipc pipe; the engine stops,
               finalizes and exits on its own. */
            send_gsr_control_command(GSR_REPLAY_IPC_PIPE, "stop");
#else
            kill(gpu_screen_recorder_process, SIGINT);
#endif
#ifdef _WIN32
            wait_gsr_process_exit(gpu_screen_recorder_process, 15000);
#else
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
#endif

            close_gpu_screen_recorder_output();

            gpu_screen_recorder_process = -1;
            recording_status = RecordingStatus::NONE;
            replay_save_duration_min = 0;
            update_ui_replay_stopped();
            replay_launched_manually = false;

            if(led_indicator)
                led_indicator->set_led(false);

            // TODO: Show this with a slight delay to make sure it doesn't show up in the video
            if(!disable_notification && config.replay_config.record_options.show_notifications)
                show_notification(TR("Replay stopped"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::REPLAY);

            return true;
        }

        if(config.replay_config.record_options.record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [disable_notification, launched_manually, this]() {
                on_press_start_replay(disable_notification, true, launched_manually);
            };
            return false;
        }

        if(config.replay_config.record_options.record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [disable_notification, launched_manually, this]() {
                on_press_start_replay(disable_notification, true, launched_manually);
            };
            return false;
        }

        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        recording_capture_target = get_capture_target(config.replay_config.record_options.record_area_option, capture_options);
        if(!validate_capture_target(config.replay_config.record_options.record_area_option, capture_options)) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), TR("Failed to start replay, capture target \"%s\" is invalid. Please change capture target in settings"), recording_capture_target.c_str());
            show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
            return false;
        }

        // TODO: Validate input, fallback to valid values
        const std::string fps = std::to_string(config.replay_config.record_options.fps);
        const std::string video_bitrate = std::to_string(config.replay_config.record_options.video_bitrate);
        const std::string output_directory = config.replay_config.save_directory;
        const std::vector<std::string> audio_tracks = create_audio_tracks_cli_args(config.replay_config.record_options.audio_tracks_list, gsr_info);
        const std::string framerate_mode = get_framerate_mode_validate(config.replay_config.record_options, gsr_info);
        const std::string replay_time = std::to_string(config.replay_config.replay_time);
        const char *container = config.replay_config.container.c_str();
        const char *video_codec = config.replay_config.record_options.video_codec.c_str();
        const char *encoder = "gpu";
        choose_video_codec_and_container_with_fallback(gsr_info, config.replay_config.record_options, &video_codec, &container, &encoder);

        char size[64];
        size[0] = '\0';
        if(config.replay_config.record_options.record_area_option == "focused")
            snprintf(size, sizeof(size), "%dx%d", (int)config.replay_config.record_options.record_area_width, (int)config.replay_config.record_options.record_area_height);

        if(config.replay_config.record_options.record_area_option != "focused" && config.replay_config.record_options.change_video_resolution)
            snprintf(size, sizeof(size), "%dx%d", (int)config.replay_config.record_options.video_width, (int)config.replay_config.record_options.video_height);

        const std::string capture_source_arg = compose_capture_source_arg(recording_capture_target, config.replay_config.record_options);

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", capture_source_arg.c_str(),
            "-c", container,
            "-ac", config.replay_config.record_options.audio_codec.c_str(),
            "-cursor", config.replay_config.record_options.record_cursor ? "yes" : "no",
            "-cr", config.replay_config.record_options.color_range.c_str(),
            "-fm", framerate_mode.c_str(),
            "-k", video_codec,
            "-encoder", encoder,
            "-f", fps.c_str(),
            "-r", replay_time.c_str(),
            "-v", "no",
            "-o", output_directory.c_str()
        };

#ifdef _WIN32
        /* Windows: control (save-replay/stop) goes over the -ipc pipe via
           gsr-cli; POSIX signals do not exist and kill() terminates. */
        args.push_back("-ipc");
        args.push_back(GSR_REPLAY_IPC_PIPE);
#endif

        if(config.replay_config.restart_replay_on_save) {
            args.push_back("-restart-replay-on-save");
            args.push_back("yes");
            replay_restart_on_save = true;
        } else {
            replay_restart_on_save = false;
        }

        args.push_back("-replay-storage");
        args.push_back(config.replay_config.replay_storage.c_str());

        char region_str[128];
        add_common_gpu_screen_recorder_args(args, config.main_config, config.replay_config.record_options, audio_tracks, video_bitrate, size, region_str, sizeof(region_str), config.replay_config.record_options.record_area_option);

        args.push_back("-ro");
        args.push_back(config.record_config.save_directory.c_str());

        args.push_back(nullptr);

        current_recording_config = config;

        gpu_screen_recorder_process = exec_program(args.data(), &gpu_screen_recorder_process_output_fd);
        if(gpu_screen_recorder_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to start replay"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
            return false;
        } else {
            recording_status = RecordingStatus::REPLAY;
            update_ui_replay_started();

            if(led_indicator && config.replay_config.record_options.use_led_indicator)
                led_indicator->set_led(true);
        }

        prepare_gsr_output_for_reading();

        // TODO: Start recording after this notification has disappeared to make sure it doesn't show up in the video.
        // Make clear to the user that the recording starts after the notification is gone.
        // Maybe have the option in notification to show timer until its getting hidden, then the notification can say:
        // Starting recording in 3...
        // 2...
        // 1...
        // TODO: Do not run this is a daemon. Instead get the pid and when launching another notification close the current notification
        // program and start another one. This can also be used to check when the notification has finished by checking with waitpid NOWAIT
        // to see when the program has exit.
        if(!disable_notification && config.replay_config.record_options.show_notifications) {
            char msg[256];
            snprintf(msg, sizeof(msg), TR("Started replaying %s"), capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), false).c_str());
            show_notification(msg, short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::REPLAY, recording_capture_target.c_str());
        }

        if(config.replay_config.record_options.record_area_option == "portal")
            hide_ui = true;

        // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
        // selected what to capture and accepted it.
        replay_duration_clock.restart();
        return true;
    }

    void Overlay::on_press_start_record(bool finished_selection, RecordForceType force_type) {
        if(region_selector->is_started())
            return;

        switch(recording_status) {
            case RecordingStatus::NONE:
            case RecordingStatus::RECORD:
                break;
            case RecordingStatus::REPLAY: {
                if(gpu_screen_recorder_process <= 0)
                    return;

                if(!replay_recording) {
                    if(config.record_config.record_options.show_notifications)
                        show_notification(TR("Started recording in the replay session"), short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::RECORD);
                    update_ui_recording_started();

                    // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
                    // selected what to capture and accepted it.
                    recording_duration_clock.restart();
                    update_upause_status();

                    if(led_indicator) {
                        if(config.record_config.record_options.use_led_indicator) {
                            if(!current_recording_config.replay_config.record_options.use_led_indicator)
                                led_indicator->set_led(true);
                            else
                                led_indicator->blink();
                        }
                    }
                }

                replay_recording = true;
#ifdef _WIN32
                send_gsr_control_command(GSR_REPLAY_IPC_PIPE, "toggle-replay-recording");
#else
                kill(gpu_screen_recorder_process, SIGRTMIN);
#endif
                return;
            }
            case RecordingStatus::STREAM: {
                if(gpu_screen_recorder_process <= 0)
                    return;

                if(!replay_recording) {
                    if(config.record_config.record_options.show_notifications)
                        show_notification(TR("Started recording in the streaming session"), short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::RECORD);
                    update_ui_recording_started();

                    // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
                    // selected what to capture and accepted it.
                    recording_duration_clock.restart();
                    update_upause_status();

                    if(led_indicator) {
                        if(config.record_config.record_options.use_led_indicator) {
                            if(!current_recording_config.streaming_config.record_options.use_led_indicator)
                                led_indicator->set_led(true);
                            else
                                led_indicator->blink();
                        }
                    }
                }

                replay_recording = true;
#ifdef _WIN32
                send_gsr_control_command(GSR_STREAM_IPC_PIPE, "toggle-replay-recording");
#else
                kill(gpu_screen_recorder_process, SIGRTMIN);
#endif
                return;
            }
        }

        if(gpu_screen_recorder_process > 0) {
#ifdef _WIN32
            /* Windows: graceful stop over the -ipc pipe; gsr-cli is detached
               and the engine stops, finalizes the file and prints the
               "Video_..." path to stdout before exiting. The output pipe
               must stay open until after drain_gsr_process_output(). */
            send_gsr_control_command(GSR_RECORD_IPC_PIPE, "stop");
#else
            kill(gpu_screen_recorder_process, SIGINT);
#endif
#ifdef _WIN32
            const int exit_code = wait_gsr_process_exit(gpu_screen_recorder_process, 15000);
            /* The update loop is blocked above, so drain the engine's stdout
               now to pick up the final filepath line. */
            drain_gsr_process_output();
            if(record_filepath.empty())
                on_stop_recording(exit_code, record_filepath);
#else
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            } else {
                int exit_code = -1;
                if(WIFEXITED(status))
                    exit_code = WEXITSTATUS(status);
                if(record_filepath.empty())
                    on_stop_recording(exit_code, record_filepath);
            }
#endif

            close_gpu_screen_recorder_output();

            gpu_screen_recorder_process = -1;
            recording_status = RecordingStatus::NONE;
            update_ui_recording_stopped();
            update_upause_status();
            record_filepath.clear();

            if(led_indicator)
                led_indicator->set_led(false);
            return;
        }

        update_upause_status();

        std::string record_area_option;
        switch(force_type) {
            case RecordForceType::NONE:
                record_area_option = config.record_config.record_options.record_area_option;
                break;
            case RecordForceType::REGION:
                record_area_option = "region";
                break;
            case RecordForceType::WINDOW:
                record_area_option = gsr_info.system_info.display_server == DisplayServer::X11 ? "window" : "portal";
                break;
        }

        if(record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_start_record(true, force_type);
            };
            return;
        }

        if(record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_start_record(true, force_type);
            };
            return;
        }

        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        recording_capture_target = get_capture_target(record_area_option, capture_options);
        if(!validate_capture_target(record_area_option, capture_options)) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), TR("Failed to start recording, capture target \"%s\" is invalid. Please change capture target in settings"), recording_capture_target.c_str());
            show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
            return;
        }

        record_filepath.clear();

        // TODO: Validate input, fallback to valid values
        const std::string fps = std::to_string(config.record_config.record_options.fps);
        const std::string video_bitrate = std::to_string(config.record_config.record_options.video_bitrate);
        const std::string output_file = config.record_config.save_directory + "/Video_" + get_date_str() + "." + container_to_file_extension(config.record_config.container.c_str());
        const std::vector<std::string> audio_tracks = create_audio_tracks_cli_args(config.record_config.record_options.audio_tracks_list, gsr_info);
        const std::string framerate_mode = get_framerate_mode_validate(config.record_config.record_options, gsr_info);
        const char *container = config.record_config.container.c_str();
        const char *video_codec = config.record_config.record_options.video_codec.c_str();
        const char *encoder = "gpu";
        choose_video_codec_and_container_with_fallback(gsr_info, config.record_config.record_options, &video_codec, &container, &encoder);

        char size[64];
        size[0] = '\0';
        if(record_area_option == "focused")
            snprintf(size, sizeof(size), "%dx%d", (int)config.record_config.record_options.record_area_width, (int)config.record_config.record_options.record_area_height);

        if(record_area_option != "focused" && config.record_config.record_options.change_video_resolution)
            snprintf(size, sizeof(size), "%dx%d", (int)config.record_config.record_options.video_width, (int)config.record_config.record_options.video_height);

        const std::string capture_source_arg = compose_capture_source_arg(recording_capture_target, config.record_config.record_options);

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", capture_source_arg.c_str(),
            "-c", container,
            "-ac", config.record_config.record_options.audio_codec.c_str(),
            "-cursor", config.record_config.record_options.record_cursor ? "yes" : "no",
            "-cr", config.record_config.record_options.color_range.c_str(),
            "-fm", framerate_mode.c_str(),
            "-k", video_codec,
            "-encoder", encoder,
            "-f", fps.c_str(),
            "-v", "no",
            "-o", output_file.c_str()
        };

#ifdef _WIN32
        /* Windows: stop/pause go over the -ipc pipe via gsr-cli. */
        args.push_back("-ipc");
        args.push_back(GSR_RECORD_IPC_PIPE);
#endif

        const std::string hotkey_window_capture_portal_session_token_filepath = get_config_dir() + "/gsr-ui-window-capture-token";
        if(record_area_option == "portal") {
            hide_ui = true;
            if(force_type == RecordForceType::WINDOW) {
                args.push_back("-portal-session-token-filepath");
                args.push_back(hotkey_window_capture_portal_session_token_filepath.c_str());
            }
        }

        char region_str[128];
        add_common_gpu_screen_recorder_args(args, config.main_config, config.record_config.record_options, audio_tracks, video_bitrate, size, region_str, sizeof(region_str), record_area_option, force_type);

        args.push_back(nullptr);

        current_recording_config = config;

        record_filepath = output_file;
        gpu_screen_recorder_process = exec_program(args.data(), &gpu_screen_recorder_process_output_fd);
        if(gpu_screen_recorder_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to start recording"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
            return;
        } else {
            recording_status = RecordingStatus::RECORD;
            update_ui_recording_started();

            if(led_indicator && config.record_config.record_options.use_led_indicator)
                led_indicator->set_led(true);
        }

        prepare_gsr_output_for_reading();

        // TODO: Start recording after this notification has disappeared to make sure it doesn't show up in the video.
        // Make clear to the user that the recording starts after the notification is gone.
        // Maybe have the option in notification to show timer until its getting hidden, then the notification can say:
        // Starting recording in 3...
        // 2...
        // 1...
        if(config.record_config.record_options.show_notifications) {
            char msg[256];
            snprintf(msg, sizeof(msg), TR("Started recording %s"), capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), false).c_str());
            show_notification(msg, short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::RECORD, recording_capture_target.c_str());
        }

        // TODO: This will be incorrect if the user uses portal capture, as capture wont start until the user has
        // selected what to capture and accepted it.
        recording_duration_clock.restart();
    }

    static std::string streaming_get_url(const Config &config) {
        std::string url;
        fprintf(stderr, "streaming service: %s\n", config.streaming_config.streaming_service.c_str());
        if(config.streaming_config.streaming_service == "twitch") {
            url += "rtmp://live.twitch.tv/app/";
            url += config.streaming_config.twitch.stream_key;
        } else if(config.streaming_config.streaming_service == "youtube") {
            url += "rtmp://a.rtmp.youtube.com/live2/";
            url += config.streaming_config.youtube.stream_key;
        } else if(config.streaming_config.streaming_service == "rumble") {
            url += "rtmp://rtmp.rumble.com/live/";
            url += config.streaming_config.rumble.stream_key;
        } else if(config.streaming_config.streaming_service == "kick") {
            url += config.streaming_config.kick.stream_url;
            if(!url.empty() && url.back() != '/')
                url += "/";
            url += "app/";
            url += config.streaming_config.kick.stream_key;
        } else if(config.streaming_config.streaming_service == "custom") {
            url = config.streaming_config.custom.url;
            if(url.size() >= 7 && strncmp(url.c_str(), "rtmp://", 7) == 0)
            {}
            else if(url.size() >= 8 && strncmp(url.c_str(), "rtmps://", 8) == 0)
            {}
            else if(url.size() >= 7 && strncmp(url.c_str(), "rtsp://", 7) == 0)
            {}
            else if(url.size() >= 6 && strncmp(url.c_str(), "srt://", 6) == 0)
            {}
            else if(url.size() >= 7 && strncmp(url.c_str(), "http://", 7) == 0)
            {}
            else if(url.size() >= 8 && strncmp(url.c_str(), "https://", 8) == 0)
            {}
            else if(url.size() >= 6 && strncmp(url.c_str(), "tcp://", 6) == 0)
            {}
            else if(url.size() >= 6 && strncmp(url.c_str(), "udp://", 6) == 0)
            {}
            else
                url = "rtmp://" + url;

            if(!url.empty() && url.back() != '/' && url.back() != '=' && !config.streaming_config.custom.key.empty())
                url += "/";

            url += config.streaming_config.custom.key;
        }
        return url;
    }

    void Overlay::on_press_start_stream(bool finished_selection) {
        if(region_selector->is_started())
            return;

        switch(recording_status) {
            case RecordingStatus::NONE:
            case RecordingStatus::STREAM:
                break;
            case RecordingStatus::REPLAY:
                show_notification(TR("Unable to start streaming when replay is turned on. Turn off replay before starting streaming."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::REPLAY, nullptr, NotificationLevel::ERROR);
                return;
            case RecordingStatus::RECORD:
                show_notification(TR("Unable to start streaming when recording. Stop recording before starting streaming."), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::RECORD, nullptr, NotificationLevel::ERROR);
                return;
        }

        update_upause_status();

        if(gpu_screen_recorder_process > 0) {
#ifdef _WIN32
            send_gsr_control_command(GSR_STREAM_IPC_PIPE, "stop");
#else
            kill(gpu_screen_recorder_process, SIGINT);
#endif
#ifdef _WIN32
            wait_gsr_process_exit(gpu_screen_recorder_process, 15000);
#else
            int status;
            if(waitpid(gpu_screen_recorder_process, &status, 0) == -1) {
                perror("waitpid failed");
                /* Ignore... */
            }
#endif

            close_gpu_screen_recorder_output();

            gpu_screen_recorder_process = -1;
            recording_status = RecordingStatus::NONE;
            update_ui_streaming_stopped();

            if(led_indicator)
                led_indicator->set_led(false);

            // TODO: Show this with a slight delay to make sure it doesn't show up in the video
            if(config.streaming_config.record_options.show_notifications)
                show_notification(TR("Streaming has stopped"), notification_timeout_seconds, mgl::Color(255, 255, 255), get_color_theme().tint_color, NotificationType::STREAM);
            return;
        }

        if(config.streaming_config.record_options.record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [this]() {
                on_press_start_stream(true);
            };
            return;
        }

        if(config.streaming_config.record_options.record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [this]() {
                on_press_start_stream(true);
            };
            return;
        }

        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        recording_capture_target = get_capture_target(config.streaming_config.record_options.record_area_option, capture_options);
        if(!validate_capture_target(config.streaming_config.record_options.record_area_option, capture_options)) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), TR("Failed to start streaming, capture target \"%s\" is invalid. Please change capture target in settings"), recording_capture_target.c_str());
            show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
            return;
        }

        // TODO: Validate input, fallback to valid values
        const std::string fps = std::to_string(config.streaming_config.record_options.fps);
        const std::string video_bitrate = std::to_string(config.streaming_config.record_options.video_bitrate);
        std::vector<std::string> audio_tracks = create_audio_tracks_cli_args(config.streaming_config.record_options.audio_tracks_list, gsr_info);
        // This isn't possible unless the user modified the config file manually,
        // But we check it anyways as streaming on some sites can fail if there is more than one audio track
        if(audio_tracks.size() > 1)
            audio_tracks.resize(1);
        const std::string framerate_mode = get_framerate_mode_validate(config.streaming_config.record_options, gsr_info);
        const char *container = "flv";
        if(config.streaming_config.streaming_service == "custom")
            container = config.streaming_config.custom.container.c_str();
        const char *video_codec = config.streaming_config.record_options.video_codec.c_str();
        const char *encoder = "gpu";
        choose_video_codec_and_container_with_fallback(gsr_info, config.streaming_config.record_options, &video_codec, &container, &encoder);

        const std::string url = streaming_get_url(config);
        if(config.streaming_config.streaming_service == "rumble" || config.streaming_config.streaming_service == "kick") {
            fprintf(stderr, "Info: forcing video codec to h264 as rumble/kick supports only h264\n");
            video_codec = "h264"; // TODO: Vulkan
        }

        char size[64];
        size[0] = '\0';
        if(config.streaming_config.record_options.record_area_option == "focused")
            snprintf(size, sizeof(size), "%dx%d", (int)config.streaming_config.record_options.record_area_width, (int)config.streaming_config.record_options.record_area_height);

        if(config.streaming_config.record_options.record_area_option != "focused" && config.streaming_config.record_options.change_video_resolution)
            snprintf(size, sizeof(size), "%dx%d", (int)config.streaming_config.record_options.video_width, (int)config.streaming_config.record_options.video_height);

        const std::string capture_source_arg = compose_capture_source_arg(recording_capture_target, config.streaming_config.record_options);

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", capture_source_arg.c_str(),
            "-c", container,
            "-ac", config.streaming_config.record_options.audio_codec.c_str(),
            "-cursor", config.streaming_config.record_options.record_cursor ? "yes" : "no",
            "-cr", config.streaming_config.record_options.color_range.c_str(),
            "-fm", framerate_mode.c_str(),
            "-k", video_codec,
            "-encoder", encoder,
            "-f", fps.c_str(),
            "-v", "no",
            "-o", url.c_str()
        };

#ifdef _WIN32
        /* Windows: stop/toggle-replay-recording go over the -ipc pipe. */
        args.push_back("-ipc");
        args.push_back(GSR_STREAM_IPC_PIPE);
#endif

        char region_str[128];
        add_common_gpu_screen_recorder_args(args, config.main_config, config.streaming_config.record_options, audio_tracks, video_bitrate, size, region_str, sizeof(region_str), config.streaming_config.record_options.record_area_option);

        args.push_back("-ro");
        args.push_back(config.record_config.save_directory.c_str());

        args.push_back(nullptr);

        current_recording_config = config;

        gpu_screen_recorder_process = exec_program(args.data(), &gpu_screen_recorder_process_output_fd);
        if(gpu_screen_recorder_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to start streaming"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::STREAM, nullptr, NotificationLevel::ERROR);
            return;
        } else {
            recording_status = RecordingStatus::STREAM;
            update_ui_streaming_started();

            if(led_indicator && config.streaming_config.record_options.use_led_indicator)
                led_indicator->set_led(true);
        }

        prepare_gsr_output_for_reading();

        // TODO: Start recording after this notification has disappeared to make sure it doesn't show up in the video.
        // Make clear to the user that the recording starts after the notification is gone.
        // Maybe have the option in notification to show timer until its getting hidden, then the notification can say:
        // Starting recording in 3...
        // 2...
        // 1...
        // TODO: Do not run this is a daemon. Instead get the pid and when launching another notification close the current notification
        // program and start another one. This can also be used to check when the notification has finished by checking with waitpid NOWAIT
        // to see when the program has exit.
        if(config.streaming_config.record_options.show_notifications) {
            char msg[256];
            snprintf(msg, sizeof(msg), TR("Started streaming %s"), capture_target_get_notification_name(x11_dpy, recording_capture_target.c_str(), false).c_str());
            show_notification(msg, short_notification_timeout_seconds, get_color_theme().tint_color, get_color_theme().tint_color, NotificationType::STREAM, recording_capture_target.c_str());
        }

        if(config.streaming_config.record_options.record_area_option == "portal")
            hide_ui = true;
    }

    void Overlay::on_press_take_screenshot(bool finished_selection, ScreenshotForceType force_type) {
        if(region_selector->is_started())
            return;

        if(gpu_screen_recorder_screenshot_process > 0) {
            fprintf(stderr, "Error: failed to take screenshot, another screenshot is currently being saved\n");
            return;
        }

        std::string record_area_option;
        switch(force_type) {
            case ScreenshotForceType::NONE:
                record_area_option = config.screenshot_config.record_area_option;
                break;
            case ScreenshotForceType::REGION:
                record_area_option = "region";
                break;
            case ScreenshotForceType::WINDOW:
                record_area_option = gsr_info.system_info.display_server == DisplayServer::X11 ? "window" : "portal";
                break;
        }

        if(record_area_option == "region" && !finished_selection) {
            start_region_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_take_screenshot(true, force_type);
            };
            return;
        }

        if(record_area_option == "window" && !finished_selection) {
            start_window_capture = true;
            on_region_selected = [this, force_type]() {
                on_press_take_screenshot(true, force_type);
            };
            return;
        }

        const SupportedCaptureOptions capture_options = get_supported_capture_options(gsr_info);
        screenshot_capture_target = get_capture_target(record_area_option, capture_options);
        if(!validate_capture_target(record_area_option, capture_options)) {
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg), TR("Failed to take a screenshot, capture target \"%s\" is invalid. Please change capture target in settings"), screenshot_capture_target.c_str());
            show_notification(err_msg, notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::SCREENSHOT, nullptr, NotificationLevel::ERROR);
            return;
        }

        // TODO: Validate input, fallback to valid values
        std::string output_file;
        if(config.screenshot_config.save_screenshot_to_disk)
            output_file = config.screenshot_config.save_directory + "/Screenshot_" + get_date_str() + "." + config.screenshot_config.image_format; // TODO: Validate image format
        else
            output_file = "/tmp/gsr_ui_clipboard_screenshot." + config.screenshot_config.image_format;

        const bool capture_cursor = force_type == ScreenshotForceType::NONE && config.screenshot_config.record_cursor;

        std::vector<const char*> args = {
            "gpu-screen-recorder", "-w", screenshot_capture_target.c_str(),
            "-cursor", capture_cursor ? "yes" : "no",
            "-v", "no",
            "-q", config.screenshot_config.image_quality.c_str(),
            "-o", output_file.c_str()
        };

        char size[64];
        size[0] = '\0';
        if(config.screenshot_config.change_image_resolution) {
            snprintf(size, sizeof(size), "%dx%d", (int)config.screenshot_config.image_width, (int)config.screenshot_config.image_height);
            args.push_back("-s");
            args.push_back(size);
        }

        if(config.screenshot_config.restore_portal_session && force_type != ScreenshotForceType::WINDOW) {
            args.push_back("-restore-portal-session");
            args.push_back("yes");
        }

        const std::string hotkey_window_capture_portal_session_token_filepath = get_config_dir() + "/gsr-ui-window-capture-token";
        if(record_area_option == "portal") {
            hide_ui = true;
            if(force_type == ScreenshotForceType::WINDOW) {
                args.push_back("-portal-session-token-filepath");
                args.push_back(hotkey_window_capture_portal_session_token_filepath.c_str());
            }
        }

        char region_str[128];
        if(record_area_option == "region")
            add_region_command(args, region_str, sizeof(region_str));

        args.push_back(nullptr);

        if(clipboard)
            clipboard->set_current_file("", Clipboard::FileType::JPG);

        screenshot_filepath = output_file;
        gpu_screen_recorder_screenshot_process = exec_program(args.data(), nullptr);
        if(gpu_screen_recorder_screenshot_process == -1) {
            show_notification(TR("Failed to launch gpu-screen-recorder to take a screenshot"), notification_error_timeout_seconds, mgl::Color(255, 0, 0), mgl::Color(255, 0, 0), NotificationType::SCREENSHOT, nullptr, NotificationLevel::ERROR);
        }
    }

    bool Overlay::update_compositor_texture(const Monitor &monitor) {
#ifndef _WIN32
        window_texture_deinit(&window_texture);
#endif
        window_texture_sprite.set_texture(nullptr);
        screenshot_texture.clear();
        screenshot_sprite.set_texture(nullptr);

#ifndef _WIN32
        if(gsr_info.system_info.display_server != DisplayServer::X11 || wayland_native_overlay)
            return false;

        Display *display = (Display*)mgl_get_context()->connection;
        if(is_compositor_running(display, 0))
            return false;

        bool window_texture_loaded = false;
        Window focused_window = get_focused_window(display, WindowCaptureType::CURSOR);
        if(!focused_window)
            focused_window = get_focused_window(display, WindowCaptureType::FOCUSED);
        if(focused_window && is_window_fullscreen_on_monitor(display, focused_window, monitor))
            window_texture_loaded = window_texture_init(&window_texture, display, mgl_window_get_egl_display(window->internal_window()), focused_window, egl_funcs) == 0;

        if(window_texture_loaded && window_texture.texture_id) {
            window_texture_texture = mgl::Texture(window_texture.texture_id, MGL_TEXTURE_FORMAT_RGB);
            window_texture_sprite.set_texture(&window_texture_texture);
        } else {
            XImage *img = XGetImage(display, DefaultRootWindow(display), monitor.position.x, monitor.position.y, monitor.size.x, monitor.size.y, AllPlanes, ZPixmap);
            if(!img)
                fprintf(stderr, "Error: failed to take a screenshot\n");

            if(img) {
                screenshot_texture = texture_from_ximage(img);
                if(screenshot_texture.is_valid())
                    screenshot_sprite.set_texture(&screenshot_texture);
                XDestroyImage(img);
                img = NULL;
            }
        }

        return true;
#else
        (void)monitor;
        return false;
#endif
    }

    void Overlay::force_window_on_top() {
#ifdef _WIN32
        return;
#else
        if(wayland_native_overlay)
            return;

        if(force_window_on_top_clock.get_elapsed_time_seconds() >= force_window_on_top_timeout_seconds) {
            force_window_on_top_clock.restart();

            // The mgl window is on mgl's X11 connection, not x11_dpy.
            Display *mgl_display = (Display*)mgl_get_context()->connection;
            XRaiseWindow(mgl_display, (Window)window->get_system_handle());
            XFlush(mgl_display);
        }
#endif /* !_WIN32 */
    }
}
