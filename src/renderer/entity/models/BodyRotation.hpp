#pragma once
#ifndef VOLCANO_BODY_ROTATION_H
#define VOLCANO_BODY_ROTATION_H

#include <algorithm>
#include <cmath>

namespace Volcano {

// Vanilla's body-versus-head turning (LivingEntity.tickHeadTurn), in degrees.
// The head always shows the absolute look direction; the body trails it,
// following the intended move direction while moving and snapping to the
// head while swinging.
struct BodyRotation {
    float bodyYaw = 0.0f;

    static float Wrap(float degrees)
    {
        degrees = std::fmod(degrees + 180.0f, 360.0f);
        if (degrees < 0.0f) degrees += 360.0f;
        return degrees - 180.0f;
    }

    // moveYaw is only read while moving. Returns the new body yaw.
    float Update(float deltaSeconds, float headYaw, bool moving, float moveYaw, bool swinging)
    {
        if (!initialized)
        {
            bodyYaw = headYaw;
            initialized = true;
        }

        // Vanilla ticks at 20 Hz; these are its per-tick factors made frame-rate independent.
        float ticks = deltaSeconds * 20.0f;

        float target = bodyYaw;
        if (moving)
        {
            target = moveYaw;
            // Walking backwards keeps the body facing forward.
            if (std::abs(Wrap(headYaw - moveYaw)) > 95.0f) target += 180.0f;
        }
        if (swinging) target = headYaw;
        bodyYaw += Wrap(target - bodyYaw) * (1.0f - std::pow(0.7f, ticks));

        float relative = std::clamp(Wrap(headYaw - bodyYaw), -MAX_HEAD_OFFSET, MAX_HEAD_OFFSET);
        bodyYaw = headYaw - relative;
        if (std::abs(relative) > SOFT_HEAD_OFFSET) bodyYaw += relative * (1.0f - std::pow(0.8f, ticks));

        bodyYaw = Wrap(bodyYaw);
        return bodyYaw;
    }

private:
    static constexpr float MAX_HEAD_OFFSET = 75.0f;  // Head never turns further than this from the body.
    static constexpr float SOFT_HEAD_OFFSET = 50.0f; // Past this the body starts catching up.
    bool initialized = false;
};

} // namespace Volcano

#endif
