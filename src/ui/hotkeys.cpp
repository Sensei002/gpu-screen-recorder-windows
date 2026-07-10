#include "ui/hotkeys.h"
#include "common/log.h"
#include <windows.h>
#include <chrono>
#include <algorithm>

// ─── Global instance for hook callbacks ─────────────────────────────────────
static HotkeyManager* g_hotkey_instance = nullptr;

// ─── Constructor / Destructor ───────────────────────────────────────────────

HotkeyManager::HotkeyManager() {
    g_hotkey_instance = this;
}

HotkeyManager::~HotkeyManager() {
    shutdown();
}

// ─── Initialize ─────────────────────────────────────────────────────────────

bool HotkeyManager::initialize() {
    // Register default hotkeys
    Hotkey toggle_ui;
    toggle_ui.vk_code = 'Z';
    toggle_ui.modifiers = MOD_ALT;
    toggle_ui.name = "toggle_ui";
    toggle_ui.callback = [this]() {
        if (m_on_toggle_ui) m_on_toggle_ui();
    };
    register_hotkey(toggle_ui);

    Hotkey save_replay;
    save_replay.vk_code = VK_F8;
    save_replay.modifiers = 0;
    save_replay.name = "save_replay";
    save_replay.callback = [this]() {
        if (m_on_save_replay) m_on_save_replay();
    };
    register_hotkey(save_replay);

    Hotkey toggle_recording;
    toggle_recording.vk_code = VK_F9;
    toggle_recording.modifiers = 0;
    toggle_recording.name = "toggle_recording";
    toggle_recording.callback = [this]() {
        if (m_on_toggle_recording) m_on_toggle_recording();
    };
    register_hotkey(toggle_recording);

    LOG_INFO("Hotkey manager initialized");
    LOG_INFO("  Default hotkeys: Alt+Z (UI), F8 (Save Replay), F9 (Toggle Recording)");
    return true;
}

void HotkeyManager::shutdown() {
    stop_message_loop();

    if (m_keyboard_hook) {
        UnhookWindowsHookEx(m_keyboard_hook);
        m_keyboard_hook = nullptr;
    }
    if (m_mouse_hook) {
        UnhookWindowsHookEx(m_mouse_hook);
        m_mouse_hook = nullptr;
    }

    std::lock_guard<std::mutex> lock(m_hotkey_mutex);
    m_hotkeys.clear();
}

// ─── Register Hotkey ────────────────────────────────────────────────────────

bool HotkeyManager::register_hotkey(const Hotkey& hotkey) {
    std::lock_guard<std::mutex> lock(m_hotkey_mutex);

    // Check if already registered
    auto it = std::find_if(m_hotkeys.begin(), m_hotkeys.end(),
                           [&](const Hotkey& h) { return h.name == hotkey.name; });
    if (it != m_hotkeys.end()) {
        *it = hotkey; // Update
    } else {
        m_hotkeys.push_back(hotkey);
    }

    LOG_DEBUG("Hotkey registered: %s (vk=0x%X, mods=%d)",
              hotkey.name.c_str(), hotkey.vk_code, hotkey.modifiers);
    return true;
}

bool HotkeyManager::unregister_hotkey(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_hotkey_mutex);
    auto it = std::find_if(m_hotkeys.begin(), m_hotkeys.end(),
                           [&](const Hotkey& h) { return h.name == name; });
    if (it != m_hotkeys.end()) {
        m_hotkeys.erase(it);
        return true;
    }
    return false;
}

std::vector<HotkeyManager::Hotkey> HotkeyManager::registered_hotkeys() const {
    std::lock_guard<std::mutex> lock(const_cast<HotkeyManager*>(this)->m_hotkey_mutex);
    return m_hotkeys;
}

// ─── Message Loop ───────────────────────────────────────────────────────────

void HotkeyManager::run_message_loop() {
    m_running = true;
    m_message_thread_id = GetCurrentThreadId();

    // Set up low-level keyboard hook
    m_keyboard_hook = SetWindowsHookEx(WH_KEYBOARD_LL, low_level_keyboard_proc,
                                       GetModuleHandle(nullptr), 0);
    if (!m_keyboard_hook) {
        LOG_ERROR("Failed to set low-level keyboard hook");
        m_running = false;
        return;
    }

    LOG_DEBUG("Hotkey message loop started");

    // Message loop
    MSG msg;
    while (m_running.load() && GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Clean up
    if (m_keyboard_hook) {
        UnhookWindowsHookEx(m_keyboard_hook);
        m_keyboard_hook = nullptr;
    }
}

void HotkeyManager::stop_message_loop() {
    m_running = false;
    if (m_message_thread_id != 0) {
        PostThreadMessage(m_message_thread_id, WM_QUIT, 0, 0);
    }
}

// ─── Low-level Keyboard Hook ────────────────────────────────────────────────

LRESULT CALLBACK HotkeyManager::low_level_keyboard_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && g_hotkey_instance && g_hotkey_instance->m_hotkeys_enabled.load()) {
        KBDLLHOOKSTRUCT* hook = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool key_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        uint32_t modifiers = 0;
        if (GetAsyncKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
        if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000)) modifiers |= MOD_WIN;

        if (key_down) {
            g_hotkey_instance->process_key_event(
                static_cast<uint32_t>(hook->vkCode), true, modifiers);
        }
    }

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK HotkeyManager::low_level_mouse_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void HotkeyManager::process_key_event(uint32_t vk_code, bool key_down, uint32_t modifiers) {
    // Debounce: don't process same key within 200ms
    auto now = std::chrono::steady_clock::now();
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    auto last_it = m_last_key_time.find(vk_code);
    if (last_it != m_last_key_time.end() && now_ms - last_it->second < 200) {
        return;
    }
    m_last_key_time[vk_code] = now_ms;

    std::lock_guard<std::mutex> lock(m_hotkey_mutex);

    // Check registered hotkeys
    for (const auto& hotkey : m_hotkeys) {
        if (hotkey.vk_code == vk_code && hotkey.modifiers == modifiers) {
            LOG_DEBUG("Hotkey triggered: %s", hotkey.name.c_str());
            if (hotkey.callback) {
                hotkey.callback();
            }
            return;
        }
    }
}
