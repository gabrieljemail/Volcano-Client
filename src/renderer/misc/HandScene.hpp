#pragma once
#ifndef VOLCANO_HAND_SCENE_H
#define VOLCANO_HAND_SCENE_H

#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include "../entity/models/EntityJsonModel.hpp"
#include "../engine/FrameContext.hpp"

namespace Volcano {

// The CPU half of the first-person hands, shared by the Vulkan HandModel and
// the Direct3D 11 hand pass: whether to draw at all, vanilla's
// ItemInHandRenderer placement, the swing / eat / swim animations, hand sway
// and the light on the player. It produces what to draw (arm boxes by mvp,
// items by placement); each backend only submits it.
//
// Settings, read every frame:
//   Graphics.HandScale   (uint32, percent) scales the arm and the item.
//   Graphics.LeftHanded  (bool) swaps which side the main hand is on.
//   Graphics.BodyView    (bool) draws the body instead of hands.
//
// Render thread only; one instance per backend (it keeps animation state).
class HandScene {
public:
    // Hands are always drawn in front of the world: draw them in a viewport whose depth ends here.
    static constexpr float MAX_DEPTH = 0.01f;

    struct Arm {
        bool right = true;
        glm::mat4 mvp{1.0f};
    };
    struct Item {
        std::string name;
        const char* context = "firstperson_righthand";
        glm::mat4 placement{1.0f};
        glm::mat4 pivotRotation{1.0f};
        float darkness = 0.0f; // 1 - handLight
        bool glint = false;
    };
    struct Frame {
        glm::mat4 proj{1.0f}; // The items' viewProj (the hand has a projection of its own).
        float handLight = 1.0f; // Brightness at the player; the arm's shade.
        std::vector<Arm> arms;
        std::vector<Item> items;
    };

    // False when nothing should be drawn this frame (not playing, spectator,
    // third person, body view). `flipY` is Vulkan's Y-down clip space.
    bool Update(const Engine::FrameContext& frame, bool flipY, Frame& out);

    // One 4x12x4 skinned box in vanilla's Y-down model space, 1 block = 16
    // units (the first-person transform chain works in that space). Right
    // arm: boxMin (-8, 0, -2), uv (40, 16); left: (4, 0, -2), (32, 48).
    static void BuildArmGeometry(const glm::vec3& boxMin, const glm::vec2& uv,
        std::vector<EntityJsonModel::Vertex>& vertices, std::vector<uint32_t>& indices);
    static constexpr glm::vec3 RIGHT_ARM_MIN{-8.0f, 0.0f, -2.0f};
    static constexpr glm::vec2 RIGHT_ARM_UV{40.0f, 16.0f};
    static constexpr glm::vec3 LEFT_ARM_MIN{4.0f, 0.0f, -2.0f};
    static constexpr glm::vec2 LEFT_ARM_UV{32.0f, 48.0f};

private:
    float mainHandHeight = 1.0f; // 1 = raised; drops after a swing.
    // Where the current swing stands: not swinging, playing in full at the raised height until the attack
    // indicator reaches SWING_CUT_STRENGTH, cut off by it, or a swing that isn't an attack (placing, mining).
    enum class SwingMode { Idle, Playing, Cut, Plain };
    SwingMode swingMode = SwingMode::Idle;
    int64_t lastRecordNs = 0;
    float handLight = 1.0f; // Brightness at the player, eased between frames.

    // Vanilla's hand sway: the hands trail look changes (lag halves every tick)
    // and tilt by a tenth of the gap. Angles are in Minecraft's convention.
    struct Sway {
        float yaw = 0.0f, pitch = 0.0f;
        float lagYaw = 0.0f, lagPitch = 0.0f;
        bool valid = false;
    };
    Sway sway;
};

} // namespace Volcano

#endif
