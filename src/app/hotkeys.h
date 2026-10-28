#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>

enum class HotkeyAction : int {
    ToggleRecord = 1,
    SaveReplay = 2,
    Screenshot = 3,
    ToggleStream = 4,
    TogglePause = 5
};

// Сочетание: виртуальная клавиша + MOD_CONTROL / MOD_ALT / MOD_SHIFT. vk == 0 — не назначено.
struct HotkeyBind {
    UINT vk = 0;
    UINT mods = 0;
    bool operator==(const HotkeyBind&) const = default;
};

struct HotkeyBinds {
    HotkeyBind record{ VK_F9, 0 };
    HotkeyBind replay{ VK_F8, 0 };
    HotkeyBind screenshot{ VK_F10, 0 };
    HotkeyBind stream{ VK_F9, MOD_CONTROL };
    HotkeyBind pause{ VK_XBUTTON1, 0 };  // Mouse4
    bool operator==(const HotkeyBinds&) const = default;
};

// "Ctrl+Shift+F9"; для пустого — "—".
std::string HotkeyName(const HotkeyBind& b);

// Глобальные горячие клавиши через RegisterHotKey без окна: WM_HOTKEY приходит в очередь
// потока, который их зарегистрировал (главный поток с циклом кадров).
class Hotkeys {
public:
    Hotkeys(const HotkeyBinds& binds, bool record, bool replay, bool screenshot, bool stream = false,
            bool pause = false);
    ~Hotkeys();

    Hotkeys(const Hotkeys&) = delete;
    Hotkeys& operator=(const Hotkeys&) = delete;

    // Разбирает очередь сообщений и ждёт до qpcDeadline; нажатия накапливаются в флагах.
    void PumpUntil(int64_t qpcDeadline);

    bool Take(HotkeyAction a);

    // Биты 1 << HotkeyAction: сочетание не зарегистрировалось (занято другой программой).
    uint32_t Failed() const { return failed_; }

private:
    void Register(HotkeyAction a, const HotkeyBind& b, const char* what);
    void Pump();

    void PollMouse();

    uint32_t registered_ = 0;
    uint32_t failed_ = 0;
    uint32_t pending_ = 0;
    // Мышиные кнопки RegisterHotKey не берёт — опрашиваем сами, без хука в чужие процессы.
    std::array<HotkeyBind, 6> mouse_{};
    std::array<bool, 6> mouseDown_{};
};
