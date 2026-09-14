#include "ChunkParser.hpp"
#include "models/BlockRegistry.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace Volcano::ChunkParser {

namespace {

// Decodes one Paletted Container (a run of `numEntries` global-registry IDs,
// bit-packed per protocol 775's format) into the plain global ID for each
// entry. `indirectMaxBits` is the bits-per-entry cutoff above which the
// container uses the direct format (no palette array, entries ARE global
// IDs) instead of indirect (entries are local indices into a palette array
// of VarInt global IDs) — 8 for block states, 3 for biomes.
std::vector<int64_t> ReadPalettedContainer(PacketReader& reader, int numEntries, int indirectMaxBits)
{
    uint8_t bitsPerEntry = reader.ReadByte();

    // Single-valued: one VarInt holds the value for every entry, and no data
    // array follows it at all.
    if (bitsPerEntry == 0) {
        int32_t singleValue = reader.ReadVarInt();
        return std::vector<int64_t>(static_cast<size_t>(numEntries), singleValue);
    }

    std::vector<int32_t> palette;
    if (bitsPerEntry <= indirectMaxBits) {
        int32_t paletteLength = reader.ReadVarInt();
        palette.reserve(static_cast<size_t>(paletteLength));
        for (int32_t i = 0; i < paletteLength; i++) palette.push_back(reader.ReadVarInt());
    }
    // else: direct format — no palette array, data array holds global IDs.

    // bitsPerEntry is a raw byte straight off the wire (0-255) — a value
    // above 64 is never legitimate and would make entriesPerLong below
    // truncate to 0, hard-crashing the process on an integer divide-by-zero
    // (a CPU trap, not a catchable C++ exception). Bail with a real
    // exception instead; NetworkClient's chunk handler logs and skips it.
    if (bitsPerEntry > 64) {
        throw std::runtime_error("Paletted container has an implausible bitsPerEntry ("
            + std::to_string(bitsPerEntry) + ") — packet is likely misaligned/misidentified.");
    }

    int entriesPerLong = 64 / bitsPerEntry;

    // The data array's length is NOT sent — it's derived from the entry
    // count and bits-per-entry. Up to 1.21.4 a VarInt length preceded the
    // longs; 1.21.5 dropped it (ViaVersion's PaletteType1_21_5 computes
    // `(size + valuesPerLong - 1) / valuesPerLong` instead). Reading a
    // VarInt here consumed a byte of real packed data and then read a
    // garbage number of longs, which is what turned block state IDs into
    // repeating-nibble junk (0x1111, 0x3333, 0x8888, ...) and scattered
    // the resulting geometry across the sky.
    int longCount = (numEntries + entriesPerLong - 1) / entriesPerLong;
    std::vector<int64_t> longs(static_cast<size_t>(longCount));
    for (int i = 0; i < longCount; i++) longs[static_cast<size_t>(i)] = reader.ReadLong();

    std::vector<int64_t> values(static_cast<size_t>(numEntries), 0);

    // bitsPerEntry == 64 would make `1ULL << 64` undefined behaviour.
    uint64_t mask = bitsPerEntry >= 64 ? ~0ULL : ((1ULL << bitsPerEntry) - 1ULL);

    for (int i = 0; i < numEntries; i++) {
        int longIndex = i / entriesPerLong;
        int bitIndex = (i % entriesPerLong) * bitsPerEntry;

        int64_t raw = 0;
        if (longIndex < static_cast<int>(longs.size())) {
            raw = static_cast<int64_t>((static_cast<uint64_t>(longs[static_cast<size_t>(longIndex)]) >> bitIndex) & mask);
        }

        if (bitsPerEntry <= indirectMaxBits) {
            values[static_cast<size_t>(i)] = (raw >= 0 && raw < static_cast<int64_t>(palette.size()))
                ? palette[static_cast<size_t>(raw)] : 0;
        } else {
            values[static_cast<size_t>(i)] = raw;
        }
    }

    return values;
}

} // namespace

std::unique_ptr<Chunk> ParseChunkDataPacket(PacketReader& reader)
{
    int32_t chunkX = reader.ReadInt();
    int32_t chunkZ = reader.ReadInt();

    // Heightmaps: a self-delimiting Prefixed Array in this protocol version
    // (VarInt count, then per entry a VarInt type + VarInt-prefixed long
    // array), not an NBT compound — no NBT parsing needed to skip past it.
    int32_t heightmapCount = reader.ReadVarInt();
    for (int32_t i = 0; i < heightmapCount; i++) {
        reader.ReadVarInt(); // heightmap type, unused
        int32_t longCount = reader.ReadVarInt();
        reader.Skip(static_cast<size_t>(longCount) * 8);
    }

    reader.ReadVarInt(); // Size — sanity bound only; sections are read directly below.

    auto chunk = std::make_unique<Chunk>(chunkX, chunkZ);

    // Assumes a standard overworld dimension: 24 sections of 16, covering
    // world Y -64..319 (Chunk's local Y 0..383).
    constexpr int SECTION_COUNT = CHUNK_SIZE_Y / 16;
    for (int section = 0; section < SECTION_COUNT; section++) {
        reader.ReadShort(); // non-air block count, unused
        reader.ReadShort(); // non-air fluid count, unused

        std::vector<int64_t> blockStates = ReadPalettedContainer(reader, 4096, 8);
        int sectionBaseY = section * 16;
        for (int i = 0; i < 4096; i++) {
            // x fastest, then z, then y slowest — matches the wire format's
            // entry ordering for a section's block-state array.
            int x = i % 16;
            int z = (i / 16) % 16;
            int ylocal = i / 256;
            int32_t stateId = static_cast<int32_t>(blockStates[static_cast<size_t>(i)]);

            Block block;
            block.visualId = BlockRegistry::MapStateId(stateId);
            if (block.visualId == 0) {
                block.nonCubeVisualId = BlockRegistry::MapStateIdNonCube(stateId);
                if (block.nonCubeVisualId != 0) {
                    block.nonCubeCollidable = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId).collidable;
                }
            }
            chunk->setBlock(x, sectionBaseY + ylocal, z, block);
        }

        ReadPalettedContainer(reader, 64, 3); // biomes — parsed to stay byte-aligned, discarded.
    }

    return chunk;
}

} // namespace Volcano::ChunkParser
