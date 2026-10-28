#pragma once
#ifndef VOLCANO_SLOT_SKIPPER_H
#define VOLCANO_SLOT_SKIPPER_H

#include "VarInt.hpp"
#include <cstdint>
#include <memory>
#include <string>

namespace Volcano {

// Values pulled out of a Slot's components while skipping them. -1 = absent.
struct SlotComponentValues {
    int32_t damage = -1;
    int32_t maxDamage = -1;
};

// Skips data component payloads in a protocol Slot by walking the protodef
// type definitions in minecraft-data's protocol.json. Only scalar values
// needed for sibling-field lookups are kept; everything else is just
// consumed. Any unknown type or malformed data makes it return false.
class SlotSkipper {
public:
    // Process-wide instance, loaded lazily from the pinned protocol.json.
    static SlotSkipper& Instance();

    // Loads protocol.json from `path`; false if unreadable or malformed.
    explicit SlotSkipper(const std::string& path);
    ~SlotSkipper();

    bool IsLoaded() const;

    // Consumes `addedCount` SlotComponents then `removedCount` component
    // type ids. Returns false on any failure; the reader is then mid-slot.
    bool SkipComponents(PacketReader& reader, int32_t addedCount, int32_t removedCount, SlotComponentValues& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Volcano

#endif
