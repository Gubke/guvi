#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct HudState {
    bool visible = false;   // показывать только во время записи
    bool stopping = false;
    bool paused = false;
    double seconds = 0.0;
    double fps = 0.0;
    std::string encoder;    // "NVENC H.264"
};

// Маленький HUD в правом верхнем углу монитора: поверх всех окон, прозрачный для мыши,
// без фокуса. Окно помечено WDA_EXCLUDEFROMCAPTURE — DWM рисует его пользователю, но в
// кадры Windows.Graphics.Capture (и любой другой захват экрана) оно не попадает.
// Если исключение из захвата не поддерживается (Windows старше 10 2004), HUD не показывается,
// чтобы не оказаться в записи.
class OverlayHud {
public:
    explicit OverlayHud(HMONITOR monitor);
    ~OverlayHud();

    OverlayHud(const OverlayHud&) = delete;
    OverlayHud& operator=(const OverlayHud&) = delete;

    void Update(const HudState& s);

private:
    void ThreadMain();
    void Render(HWND hwnd, const HudState& s, bool blink);

    HMONITOR monitor_;
    std::mutex mtx_;
    HudState state_;
    std::atomic<bool> quit_{ false };
    std::atomic<DWORD> threadId_{ 0 };
    std::thread thread_;
};
