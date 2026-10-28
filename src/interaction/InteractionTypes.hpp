#pragma once
#ifndef VOLCANO_INTERACTION_TYPES_H
#define VOLCANO_INTERACTION_TYPES_H

#include <cstdint>
#include <glm/glm.hpp>

namespace Volcano {

// Protocol's own hand enum (packet_use_entity/packet_block_dig/
// packet_block_place/packet_use_item all take one) — shared by every event
// model below and by NetworkClient's Send* functions, rather than each
// redeclaring "0 = main hand" locally.
enum class Hand : int32_t {
    Main = 0,
    Off = 1,
};

// packet_block_dig's `status` field (play.toServer.types.packet_block_dig,
// resources/minecraft-data/data/pc/26.1/protocol.json) — vanilla's own
// ServerboundPlayerActionPacket.Action ordinal.
enum class DigStatus : int32_t {
    StartDigging = 0,
    CancelDigging = 1,
    FinishDigging = 2,
    DropItemStack = 3,
    DropItem = 4,
    ReleaseUseItem = 5, // Early-release of a continuous use (a drawn bow, food, a shield) — SecondaryTriggerRelease.
    SwapItemInHand = 6, // Swaps mainhand <-> offhand (vanilla's default 'F' keybind) — not wired to any input yet.
};

// Vanilla's net.minecraft.core.Direction enum ordinal — DOWN, UP, NORTH,
// SOUTH, WEST, EAST — unchanged since the digging packet's face byte was
// introduced (Beta 1.8) and reused as-is by packet_block_place's `direction`
// field. Every one of TargetedBlock::face's 6 possible unit-normal values
// maps to exactly one of these.
inline int32_t DirectionToProtocolId(const glm::ivec3& dir)
{
    if (dir.y < 0) return 0; // DOWN
    if (dir.y > 0) return 1; // UP
    if (dir.z < 0) return 2; // NORTH
    if (dir.z > 0) return 3; // SOUTH
    if (dir.x < 0) return 4; // WEST
    return 5;                // EAST
}

} // namespace Volcano

#endif
