#pragma once

#include "common/types.h"
#include <windows.h>
#include <functional>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>

// Forward declarations
struct KeyboardHook;
struct RawInputHandler;

class HotkeyManager {
public:
    HotkeyManager();
    ~HotkeyManager();

    // Initialize the hotkey system
    bool initialize();
    void shutdown();

    // Set hotkey callbacks
    using HotkeyCallback = std::function<void()>;

    void set_on_toggle_ui(HotkeyCallback cb) { m_on_toggle_ui = std::move(cb); }
    void set_on_save_replay(HotkeyCallback cb) { m_on_save_replay = std::move(cb); }
    void set_on_toggle_recording(HotkeyCallback cb) { m_on_toggle_recording = std::move(cb); }
    void set_on_take_screenshot(HotkeyCallback cb) { m_on_take_screenshot = std::move(cb); }
    void set_on_show_overlay(HotkeyCallback cb) { m_on_show_overlay = std::move(cb); }
    void set_on_hide_overlay(HotkeyCallback cb) { m_on_hide_overlay = std::move(cb); }

    // Register custom hotkey
    struct Hotkey {
        uint32_t vk_code = 0;       // Virtual key code
        uint32_t modifiers = 0;     // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
        std::string name;
        HotkeyCallback callback;
        bool registered = false;
    };

    bool register_hotkey(const Hotkey& hotkey);
    bool unregister_hotkey(const std::string& name);
    void set_hotkeys_enabled(bool enabled) { m_hotkeys_enabled = enabled; }
    bool hotkeys_enabled() const { return m_hotkeys_enabled; }

    // List registered hotkeys
    std::vector<Hotkey> registered_hotkeys() const;

    // Run the message loop (blocking)
    void run_message_loop();
    void stop_message_loop();

private:
    static LRESULT CALLBACK low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam);

    void process_key_event(uint32_t vk_code, bool key_down, uint32_t modifiers);

    // Hotkey hooks
    HHOOK m_keyboard_hook = nullptr;
    HHOOK m_mouse_hook = nullptr;

    // Registered hotkeys
    std::vector<Hotkey> m_hotkeys;
    std::mutex m_hotkey_mutex;

    // Callbacks
    HotkeyCallback m_on_toggle_ui;
    HotkeyCallback m_on_save_replay;
    HotkeyCallback m_on_toggle_recording;
    HotkeyCallback m_on_take_screenshot;
    HotkeyCallback m_on_show_overlay;
    HotkeyCallback m_on_hide_overlay;

    // State
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_hotkeys_enabled{true};
    std::thread m_message_thread;
    DWORD m_message_thread_id = 0;  // Thread ID of the message loop thread

    // Key state tracking
    std::unordered_map<uint32_t, bool> m_key_states;
    std::unordered_map<uint32_t, uint64_t> m_last_key_time;
};
