#pragma once
#ifndef VOLCANO_ENTITY_MOTION_H
#define VOLCANO_ENTITY_MOTION_H

#include <string>

namespace Volcano {

// How the client flies an entity between server updates. The server only
// sends items, projectiles and the like about once a second, so vanilla
// clients simulate them from their velocity.
struct MotionProfile {
    bool simulated = false;
    float gravity = 0.04f;        // Blocks per tick squared, applied before the move.
    float drag = 0.98f;           // Velocity multiplier per tick.
    bool groundFriction = false;  // Slides to a stop on the ground (items, orbs).
    bool stopsOnBlocks = false;   // Sticks where it hits (arrows).
};

inline MotionProfile MotionFor(const std::string& name)
{
    MotionProfile profile;
    if (name == "item" || name == "falling_block" || name == "tnt")
    {
        profile = { true, 0.04f, 0.98f, true, false };
    }
    else if (name == "experience_orb")
    {
        profile = { true, 0.03f, 0.98f, true, false };
    }
    else if (name == "arrow" || name == "spectral_arrow" || name == "trident")
    {
        profile = { true, 0.05f, 0.99f, false, true };
    }
    else if (name == "snowball" || name == "egg" || name == "ender_pearl")
    {
        profile = { true, 0.03f, 0.99f, false, true };
    }
    else if (name == "experience_bottle" || name == "potion" || name == "splash_potion" || name == "lingering_potion")
    {
        profile = { true, 0.07f, 0.99f, false, true };
    }
    return profile;
}

} // namespace Volcano

#endif
