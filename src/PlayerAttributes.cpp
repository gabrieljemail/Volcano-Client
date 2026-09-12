#include "PlayerAttributes.hpp"

namespace Volcano {

PlayerAttributes PlayerAttributes::Defaults()
{
    PlayerAttributes attrs;
    // Vanilla's real default attribute value (blocks/tick input scale, not
    // a direct speed) — see TickLoop::Tick() for the friction/acceleration
    // model this feeds into, which converges to vanilla's documented
    // 0.2159 blocks/tick (4.317 blocks/sec) walking speed.
    attrs.SetDouble("generic.movement_speed", 0.1);
    // Vanilla's real per-tick gravity constant (blocks/tick^2) — also a
    // real player attribute as of the versions that added generic.gravity.
    attrs.SetDouble("generic.gravity", 0.08);
    // Vanilla's real jump launch velocity, in blocks/tick.
    attrs.SetDouble("generic.jump_strength", 0.42);
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
