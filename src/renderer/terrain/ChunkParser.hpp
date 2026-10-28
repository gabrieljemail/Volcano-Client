#pragma once
#ifndef VOLCANO_CHUNK_PARSER_H
#define VOLCANO_CHUNK_PARSER_H

#include <memory>
#include "../../network/VarInt.hpp"
#include "models/Chunk.hpp"
#include "models/ChunkSection.hpp"

namespace Volcano::ChunkParser {

// Parses the payload of a "Chunk Data and Update Light" packet (protocol
// 775) into a Chunk: the block-state paletted containers, and the per-section
// sky/block light arrays that follow the block entities. Heightmaps and
// biomes are skipped/discarded (heightmaps are self-delimiting, not NBT, in
// this protocol version), and block entities are read only far enough to
// stay aligned with the light data after them
std::unique_ptr<Chunk> ParseChunkDataPacket(PacketReader& reader);

// Parses a standalone "Update Light" packet
PendingLightUpdate ParseLightUpdatePacket(PacketReader& reader);

} // namespace Volcano::ChunkParser

#endif
