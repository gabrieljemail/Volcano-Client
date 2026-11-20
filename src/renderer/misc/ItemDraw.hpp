#pragma once
#ifndef VOLCANO_ITEM_DRAW_H
#define VOLCANO_ITEM_DRAW_H

#include <chrono>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ItemModel.hpp"

namespace Volcano {

// The CPU half of drawing an item, shared by the Vulkan ItemRenderer and the
// Direct3D 11 one: the per-draw constants item.vert reads (push constants on
// Vulkan, a constant buffer on D3D11 - the HLSL twin has the same layout).
// 128 bytes, the Vulkan push-constant minimum.
struct ItemDrawParams {
    glm::mat4 mvp;
    glm::vec4 normalRow0; // Rows of the 3x3 normal matrix (rotation part of the display transform).
    glm::vec4 normalRow1;
    glm::vec4 normalRow2;
    glm::vec4 params; // x: 1 = flat item; y: how much of the light to take away (0 = none); z: glint time in seconds.
};
static_assert(sizeof(ItemDrawParams) == 128, "ItemDrawParams must fit the 128-byte push constant minimum");

namespace ItemDraw {

constexpr float GUI_GLINT_SECONDS = 9.0f; // A fixed glint moment for the one still frame baked into an icon.

// Seconds since the first call, for the scrolling enchantment glint.
inline float GlintSeconds()
{
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

inline void SetNormalRows(ItemDrawParams& draw, const glm::mat3& normalMatrix)
{
    draw.normalRow0 = glm::vec4(normalMatrix[0][0], normalMatrix[1][0], normalMatrix[2][0], 0.0f);
    draw.normalRow1 = glm::vec4(normalMatrix[0][1], normalMatrix[1][1], normalMatrix[2][1], 0.0f);
    draw.normalRow2 = glm::vec4(normalMatrix[0][2], normalMatrix[1][2], normalMatrix[2][2], 0.0f);
}

// `placement` * the model's display transform for `context`, then `viewProj`.
// `pivotRotation` turns the item in place about its own center, in `placement`
// space, after the display transform.
inline ItemDrawParams MakeWorld(const ItemModel& model, const std::string& context, const glm::mat4& placement,
    const glm::mat4& viewProj, const glm::mat4& pivotRotation, float darkness, float seconds)
{
    glm::mat4 display = model.DisplayMatrix(context);
    glm::vec3 center = glm::vec3(display * glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
    glm::mat4 pivot = glm::translate(glm::mat4(1.0f), center) * pivotRotation * glm::translate(glm::mat4(1.0f), -center);
    glm::mat4 modelMatrix = placement * pivot * display;
    glm::mat3 normalMatrix(modelMatrix);
    // Lights are fixed in the placement's space; strip any uniform scale.
    normalMatrix = glm::mat3(glm::normalize(normalMatrix[0]), glm::normalize(normalMatrix[1]), glm::normalize(normalMatrix[2]));

    ItemDrawParams draw{};
    draw.mvp = viewProj * modelMatrix;
    SetNormalRows(draw, normalMatrix);
    draw.params = glm::vec4(model.IsFlat() ? 1.0f : 0.0f, darkness, seconds, 0.0f);
    return draw;
}

// The model's own "gui" transform, viewed down -Z through a unit-sized
// orthographic window (one item = one 16px vanilla slot). `flipY` is Vulkan's
// Y-down clip space; Direct3D 11 passes false.
inline ItemDrawParams MakeGuiIcon(const ItemModel& model, bool flipY)
{
    glm::mat4 modelMatrix = model.DisplayMatrix("gui");
    glm::mat4 projection = glm::orthoRH_ZO(-0.5f, 0.5f, -0.5f, 0.5f, -2.0f, 2.0f);
    if (flipY) projection[1][1] *= -1.0f;

    ItemDrawParams draw{};
    draw.mvp = projection * modelMatrix;
    SetNormalRows(draw, glm::mat3(modelMatrix));
    draw.params = glm::vec4(model.IsFlat() ? 1.0f : 0.0f, 0.0f, GUI_GLINT_SECONDS, 0.0f);
    return draw;
}

} // namespace ItemDraw
} // namespace Volcano

#endif
