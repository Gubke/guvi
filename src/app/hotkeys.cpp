#include "app/hotkeys.h"
#include "common/clock.h"
#include "common/log.h"

#include <cstdio>
#include <format>
#include <thread>

namespace {

uint32_t Bit(HotkeyAction a)
{
    return 1u << static_cast<int>(a);
}

std::string KeyName(UINT vk)
{
    switch (vk) {
    case VK_XBUTTON1: return "Mouse4";
    case VK_XBUTTON2: return "Mouse5";
    case VK_MBUTTON: return "Mouse3";
    case VK_RBUTTON: return "Mouse2";
    case VK_LBUTTON: return "Mouse1";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
        return std::format("F{}", vk - VK_F1 + 1);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        return std::string(1, static_cast<char>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return std::format("Num {}", vk - VK_NUMPAD0);
    switch (vk) {
    case VK_SNAPSHOT: return "PrtSc";
    case VK_PAUSE: return "Pause";
    case VK_SCROLL: return "ScrollLock";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PgUp";
    case VK_NEXT: return "PgDn";
    case VK_SPACE: return "Space";
    case VK_TAB: return "Tab";
    case VK_MULTIPLY: return "Num *";
    case VK_ADD: return "Num +";
    case VK_SUBTRACT: return "Num -";
    case VK_DIVIDE: return "Num /";
    case VK_DECIMAL: return "Num .";
    default: break;
    }
    wchar_t buf[64]{};
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextW(static_cast<LONG>(sc << 16), buf, 64) > 0) {
        char out[128]{};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
        return out;
    }
    return std::format("0x{:02X}", vk);
}

bool IsMouseButton(UINT vk)
{
    return vk == VK_XBUTTON1 || vk == VK_XBUTTON2 || vk == VK_MBUTTON || vk == VK_RBUTTON;
}

bool ModsMatch(UINT mods)
{
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (static_cast<bool>(mods & MOD_CONTROL) != ctrl)
        return false;
    if (static_cast<bool>(mods & MOD_ALT) != alt)
        return false;
    if (static_cast<bool>(mods & MOD_SHIFT) != shift)
        return false;
    return true;
}

} // namespace

std::string HotkeyName(const HotkeyBind& b)
{
    if (!b.vk)
        return "—";
    std::string s;
    if (b.mods & MOD_CONTROL)
        s += "Ctrl+";
    if (b.mods & MOD_ALT)
        s += "Alt+";
    if (b.mods & MOD_SHIFT)
        s += "Shift+";
    return s + KeyName(b.vk);
}

Hotkeys::Hotkeys(const HotkeyBinds& binds, bool record, bool replay, bool screenshot, bool stream, bool pause)
{
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // создать очередь сообщений потока

    if (record)
        Register(HotkeyAction::ToggleRecord, binds.record, "старт/стоп записи");
    if (replay)
        Register(HotkeyAction::SaveReplay, binds.replay, "сохранить повтор");
    if (screenshot)
        Register(HotkeyAction::Screenshot, binds.screenshot, "скриншот");
    if (stream)
        Register(HotkeyAction::ToggleStream, binds.stream, "старт/стоп трансляции");
    if (pause)
        Register(HotkeyAction::TogglePause, binds.pause, "пауза");
}

Hotkeys::~Hotkeys()
{
    for (int id = 1; id <= 5; ++id)
        if (registered_ & (1u << id))
            UnregisterHotKey(nullptr, id);
}

void Hotkeys::Register(HotkeyAction a, const HotkeyBind& b, const char* what)
{
    if (!b.vk)
        return;
    const int id = static_cast<int>(a);
    const std::string name = HotkeyName(b);
    if (IsMouseButton(b.vk)) {
        mouse_[static_cast<size_t>(id)] = b;
        mouseDown_[static_cast<size_t>(id)] = (GetAsyncKeyState(b.vk) & 0x8000) != 0;
        LogInfo("  %s — %s\n", name.c_str(), what);
        return;
    }
    if (RegisterHotKey(nullptr, id, (b.mods & (MOD_CONTROL | MOD_ALT | MOD_SHIFT)) | MOD_NOREPEAT, b.vk)) {
        registered_ |= Bit(a);
        LogInfo("  %s — %s\n", name.c_str(), what);
    } else {
        failed_ |= Bit(a);
        LogWarn("  %s — %s: не удалось зарегистрировать (занято другой программой, ошибка %lu)\n", name.c_str(),
                what, GetLastError());
    }
}

void Hotkeys::PollMouse()
{
    for (int id = 1; id <= 5; ++id) {
        const HotkeyBind& b = mouse_[static_cast<size_t>(id)];
        if (!b.vk)
            continue;
        const bool down = (GetAsyncKeyState(b.vk) & 0x8000) != 0;
        if (down && !mouseDown_[static_cast<size_t>(id)] && ModsMatch(b.mods))
            pending_ |= 1u << id;
        mouseDown_[static_cast<size_t>(id)] = down;
    }
}

void Hotkeys::Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_HOTKEY && msg.wParam >= 1 && msg.wParam <= 5)
            pending_ |= 1u << static_cast<int>(msg.wParam);
    }
    PollMouse();
}

void Hotkeys::PumpUntil(int64_t qpcDeadline)
{
    const int64_t freq = QpcFrequency();
    for (;;) {
        Pump();
        const int64_t left = qpcDeadline - QpcNow();
        if (left <= 0)
            return;
        const int64_t ms = left * 1000 / freq;
        if (ms > 1)
            MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(ms - 1), QS_ALLINPUT);
        else
            std::this_thread::yield();
    }
}

bool Hotkeys::Take(HotkeyAction a)
{
    const bool set = (pending_ & Bit(a)) != 0;
    pending_ &= ~Bit(a);
    return set;
}
