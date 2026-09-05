#include "PlayerAttributes.hpp"

namespace Volcano {

PlayerAttributes PlayerAttributes::Defaults()
{
    PlayerAttributes attrs;
    // Vanilla's real walk speed, in blocks/sec.
    attrs.SetDouble("generic.movement_speed", 4.317);
    // Tuned to approximate vanilla's fall feel with a simple constant-
    // acceleration model — not derived from vanilla's actual per-tick
    // gravity-with-drag formula (0.08 blocks/tick^2, 0.98 drag/tick).
    attrs.SetDouble("generic.gravity", 32.0);
    // Initial jump velocity (blocks/sec); combined with the gravity above
    // gives a ~1.3 block jump height, close to vanilla's ~1.25 blocks.
    attrs.SetDouble("generic.jump_strength", 9.0);
    return attrs;
}

double PlayerAttributes::GetDouble(const std::string& name, double fallback) const
{
    auto it = attributes.find(name);
    if (it == attributes.end() || it->second.type != NBT::TagType::Double) return fallback;
    return std::get<double>(it->second.value);
}

void PlayerAttributes::SetDouble(const std::string& name, double value)
{
    attributes[name] = NBT::Tag{NBT::TagType::Double, value};
}

} // namespace Volcano
