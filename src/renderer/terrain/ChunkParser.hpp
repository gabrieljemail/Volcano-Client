#pragma once
#ifndef VOLCANO_CHUNK_PARSER_H
#define VOLCANO_CHUNK_PARSER_H

#include <memory>
#include "../../network/VarInt.hpp"
#include "models/Chunk.hpp"

namespace Volcano::ChunkParser {

// Parses the payload of a "Chunk Data and Update Light" packet (protocol
// 776) into a Chunk. Only the block-state paletted containers are decoded;
// heightmaps are skipped (self-delimiting, not NBT in this protocol
// version), and everything after the section data (block entities, light
// arrays) is left unread — safe, since Connection::ReadPacket already framed
// the whole payload and nothing follows in the same packet that this client
// currently uses. Assumes a standard overworld dimension (24 sections,
// world Y -64..319); other dimension heights aren't handled yet.
std::unique_ptr<Chunk> ParseChunkDataPacket(PacketReader& reader);

} // namespace Volcano::ChunkParser

#endif
