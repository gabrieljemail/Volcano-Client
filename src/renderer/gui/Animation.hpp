#pragma once
#ifndef VOLCANO_GUI_ANIMATION_H
#define VOLCANO_GUI_ANIMATION_H

#include <cmath>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <imgui.h>

namespace Volcano::GUI {

// CssEase and CssEaseOut are CSS's own ease and ease-out timing functions.
enum class Easing { Linear, EaseOutCubic, EaseInOutCubic, EaseOutBack, CssEase, CssEaseOut };

// CSS cubic-bezier(x1, y1, x2, y2): solves the curve's x(s) = x for s, then returns y(s).
inline float CubicBezier(float x1, float y1, float x2, float y2, float x)
{
    auto sample = [](float a, float b, float s)
    {
        float inverse = 1.0f - s;
        return 3.0f * inverse * inverse * s * a + 3.0f * inverse * s * s * b + s * s * s;
    };
    auto slope = [](float a, float b, float s)
    {
        float inverse = 1.0f - s;
        return 3.0f * inverse * inverse * a + 6.0f * inverse * s * (b - a) + 3.0f * s * s * (1.0f - b);
    };

    float s = x;
    for (int i = 0; i < 8; i++)
    {
        float error = sample(x1, x2, s) - x;
        float derivative = slope(x1, x2, s);
        if (std::fabs(error) < 1e-5f || std::fabs(derivative) < 1e-6f) break;
        s -= error / derivative;
    }
    return sample(y1, y2, glm::clamp(s, 0.0f, 1.0f));
}

inline float Ease(Easing easing, float t)
{
    t = glm::clamp(t, 0.0f, 1.0f);
    switch (easing)
    {
    case Easing::EaseOutCubic: return 1.0f - std::pow(1.0f - t, 3.0f);
    case Easing::EaseInOutCubic: return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
    case Easing::EaseOutBack:
    {
        constexpr float c1 = 1.70158f, c3 = c1 + 1.0f;
        return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
    }
    case Easing::CssEase: return CubicBezier(0.25f, 0.1f, 0.25f, 1.0f, t);
    case Easing::CssEaseOut: return CubicBezier(0.0f, 0.0f, 0.58f, 1.0f, t);
    default: return t;
    }
}

// A value that eases from wherever it currently is toward a target. Works for
// anything glm::mix handles (float, glm::vec2, ...). Times are seconds on any
// monotonic clock, such as ImGui::GetTime().
template<typename T>
class Animated
{
public:
    explicit Animated(T value = T{}) : from(value), target(value), current(value) {}

    // Retargeting mid-flight starts from the current value, so it never jumps.
    void SetTarget(T newTarget, float now, float newDuration = 0.12f, Easing newEasing = Easing::EaseOutCubic)
    {
        from = Get(now);
        target = newTarget;
        startTime = now;
        duration = newDuration;
        easing = newEasing;
    }

    void Snap(T value) { from = target = current = value; duration = 0.0f; }

    T Get(float now)
    {
        float t = duration <= 0.0f ? 1.0f : (now - startTime) / duration;
        current = t >= 1.0f ? target : glm::mix(from, target, Ease(easing, t));
        return current;
    }

    bool IsAnimating(float now) const { return duration > 0.0f && now - startTime < duration; }
    const T& Target() const { return target; }

private:
    T from, target, current;
    float startTime = 0.0f;
    float duration = 0.0f;
    Easing easing = Easing::EaseOutCubic;
};

// Named animated values for immediate-mode UI, where a widget has no object to
// hold its own state: Track() every frame with the value it should be at.
//   float x = animations.Track("hotbar.selection", selectedSlot, ImGui::GetTime());
template<typename T>
class AnimationSet
{
public:
    T Track(const std::string& key, T target, float now, float duration = 0.12f, Easing easing = Easing::EaseOutCubic)
    {
        auto [it, inserted] = values.try_emplace(key, Animated<T>(target));
        if (!inserted && it->second.Target() != target) it->second.SetTarget(target, now, duration, easing);
        return it->second.Get(now);
    }

    void Remove(const std::string& key) { values.erase(key); }
    void Clear() { values.clear(); }

private:
    std::unordered_map<std::string, Animated<T>> values;
};

// Position offset, scale and rotation around a pivot (fractions of the
// rectangle, 0.5 = center), plus an alpha multiplier.
struct Transform2D
{
    glm::vec2 translate{0.0f};
    glm::vec2 scale{1.0f};
    float rotation = 0.0f; // Radians.
    float alpha = 1.0f;
    glm::vec2 pivot{0.5f};
};

// AddImage with a Transform2D applied. `nearest` keeps pixel art crisp.
inline void AddImageTransformed(ImDrawList* drawList, ImTextureID texture, ImVec2 min, ImVec2 max,
    const Transform2D& transform, ImVec2 uv0 = ImVec2(0.0f, 0.0f), ImVec2 uv1 = ImVec2(1.0f, 1.0f),
    ImU32 tint = IM_COL32_WHITE, bool nearest = true)
{
    glm::vec2 size(max.x - min.x, max.y - min.y);
    glm::vec2 pivot = glm::vec2(min.x, min.y) + size * transform.pivot;
    float cosine = std::cos(transform.rotation), sine = std::sin(transform.rotation);

    auto place = [&](float cornerX, float cornerY)
    {
        glm::vec2 offset = (glm::vec2(cornerX, cornerY) - pivot) * transform.scale;
        glm::vec2 rotated(offset.x * cosine - offset.y * sine, offset.x * sine + offset.y * cosine);
        glm::vec2 point = pivot + rotated + transform.translate;
        return ImVec2(point.x, point.y);
    };

    ImU32 alphaMasked = (tint & ~IM_COL32_A_MASK)
        | (static_cast<ImU32>(glm::clamp(static_cast<float>((tint >> IM_COL32_A_SHIFT) & 0xFF) * transform.alpha, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);

    ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
    if (nearest) drawList->AddCallback(platformIO.DrawCallback_SetSamplerNearest, nullptr);
    drawList->AddImageQuad(texture, place(min.x, min.y), place(max.x, min.y), place(max.x, max.y), place(min.x, max.y),
        uv0, ImVec2(uv1.x, uv0.y), uv1, ImVec2(uv0.x, uv1.y), alphaMasked);
    if (nearest) drawList->AddCallback(platformIO.DrawCallback_SetSamplerLinear, nullptr);
}

} // namespace Volcano::GUI

#endif
