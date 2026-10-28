#pragma once

#include <algorithm>

// Положение картинки веб-камеры в кадре. Одна формула для движка (композитинг) и для окна настроек (превью),
// чтобы то, что видно в превью, совпадало с записью.
struct WebcamLayout {
    float size = 25.0f;  // ширина картинки, % ширины кадра (5–100)
    float x = 100.0f;    // 0 — у левого края, 100 — у правого
    float y = 100.0f;    // 0 — у верхнего края, 100 — у нижнего
};

struct WebcamRect {
    float x = 0, y = 0, w = 0, h = 0;
};

inline constexpr float kWebcamMinSize = 5.0f;

inline WebcamRect PlaceWebcam(float frameW, float frameH, float camW, float camH, const WebcamLayout& l)
{
    if (frameW <= 0 || frameH <= 0 || camW <= 0 || camH <= 0)
        return {};
    const float aspect = camH / camW;
    float w = frameW * std::clamp(l.size, kWebcamMinSize, 100.0f) / 100.0f;
    float h = w * aspect;
    if (h > frameH) {
        h = frameH;
        w = h / aspect;
    }
    WebcamRect r;
    r.w = w;
    r.h = h;
    r.x = (frameW - w) * std::clamp(l.x, 0.0f, 100.0f) / 100.0f;
    r.y = (frameH - h) * std::clamp(l.y, 0.0f, 100.0f) / 100.0f;
    return r;
}

// Обратное преобразование: прямоугольник (после перетаскивания мышью) → проценты.
inline WebcamLayout LayoutFromRect(float frameW, float frameH, const WebcamRect& r)
{
    WebcamLayout l;
    l.size = std::clamp(r.w / frameW * 100.0f, kWebcamMinSize, 100.0f);
    const float freeX = frameW - r.w;
    const float freeY = frameH - r.h;
    l.x = freeX > 0.5f ? std::clamp(r.x / freeX * 100.0f, 0.0f, 100.0f) : 0.0f;
    l.y = freeY > 0.5f ? std::clamp(r.y / freeY * 100.0f, 0.0f, 100.0f) : 0.0f;
    return l;
}
