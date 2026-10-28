#pragma once
#ifndef VOLCANO_ENTITY_REGISTRY_H
#define VOLCANO_ENTITY_REGISTRY_H

#include <cstdint>
#include <string>

namespace Volcano::EntityRegistry {

// Which procedural mesh/pipeline EntityRenderer draws a type with
enum class ShapeKind {
    Cube,      // Flat-tinted placeholder box
    Humanoid,  // Head/body/arms/legs skeleton
    Quadruped, // Body + head + 4 legs, generic proportions
    Golem,     // Iron golem's own blocky proportions
    Pig,       // Real vanilla-derived pig proportions
    Cow,       // Real vanilla-derived cow proportions
    Block,     // A textured block cube (falling_block, tnt)
};

// One entity type's static metadata, straight from minecraft-data's
// entities.json (see EntityRegistry.cpp for the version pinned).
struct EntityTypeInfo {
    std::string name;        // e.g. "cow"
    std::string displayName; // e.g. "Cow".
    float width = 0.0f;
    float height = 0.0f;
    std::string category;    // e.g. "Passive mobs"
    std::string entityKind;  // minecraft-data's "type" field
                              // the one field that reliably says whether a type is alive at all (see
                              // ClassifyShape), so kept here too for anything else that needs the same check.
    ShapeKind shape = ShapeKind::Cube; // See ShapeKind above.

    // Whether this is one of vanilla's LivingEntity subclasses (players,
    // mobs, armor stands) rather than an object (item, boat, arrow, ...) -
    // derived from entityKind. A critical hit needs a living target
    // (vanilla's Player.canCriticalAttack).
    bool IsLiving() const
    {
        return entityKind == "player" || entityKind == "mob" || entityKind == "animal" || entityKind == "hostile"
            || entityKind == "water_creature" || entityKind == "passive" || entityKind == "ambient"
            || entityKind == "living";
    }
};

// Parses minecraft-data's entities.json once at startup, keyed by its "id"
// field - the same id sent as the entity type in the protocol's Spawn
// Entity packet. Must be called once before any lookup; not thread-safe
// with itself, but read-only (and thread-safe to call from any thread)
// after it returns.
void Init();

// Returns the static metadata for a protocol entity-type id, or nullptr if
// Init() hasn't run or the id isn't in the loaded table.
const EntityTypeInfo* Lookup(uint32_t protocolTypeId);

} // namespace Volcano::EntityRegistry

#endif
