#pragma once
#ifndef VOLCANO_ENTITY_REGISTRY_H
#define VOLCANO_ENTITY_REGISTRY_H

#include <cstdint>
#include <string>

namespace Volcano::EntityRegistry {

// One entity type's static metadata, straight from minecraft-data's
// entities.json (see EntityRegistry.cpp for the version pinned).
struct EntityTypeInfo {
    std::string name;        // e.g. "cow" — matches the resource-pack/protocol name.
    std::string displayName; // e.g. "Cow".
    float width = 0.0f;
    float height = 0.0f;
    std::string category;    // e.g. "Passive mobs".
};

// Parses minecraft-data's entities.json once at startup, keyed by its "id"
// field — the same id sent as the entity type in the protocol's Spawn
// Entity packet. Must be called once before any lookup; not thread-safe
// with itself, but read-only (and thread-safe to call from any thread)
// after it returns.
void Init();

// Returns the static metadata for a protocol entity-type id, or nullptr if
// Init() hasn't run or the id isn't in the loaded table.
const EntityTypeInfo* Lookup(uint32_t protocolTypeId);

} // namespace Volcano::EntityRegistry

#endif
