#include "ChunkParser.hpp"
#include "models/BlockRegistry.hpp"
#include "../../network/NBT.hpp"
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Volcano::ChunkParser {

namespace {

// A BitSet field (Sky/Block Light Mask)
std::vector<uint64_t> ReadBitSet(PacketReader& reader)
{
    int32_t longCount = reader.ReadVarInt();
    std::vector<uint64_t> longs(static_cast<size_t>(longCount));
    for (int32_t i = 0; i < longCount; i++) longs[static_cast<size_t>(i)] = static_cast<uint64_t>(reader.ReadLong());
    return longs;
}

bool BitSetTest(const std::vector<uint64_t>& bits, int index)
{
    size_t word = static_cast<size_t>(index) / 64;
    return word < bits.size() && ((bits[word] >> (static_cast<unsigned>(index) % 64)) & 1ULL) != 0;
}

// One section's worth of light: 4096 half-bytes packed two-per-byte, low
// nibble first
std::vector<uint8_t> ReadLightArray(PacketReader& reader)
{
    int32_t length = reader.ReadVarInt(); // always 2048 on the wire; still length-prefixed
    std::vector<uint8_t> data(static_cast<size_t>(length));
    reader.ReadBytes(data.data(), static_cast<size_t>(length));
    return data;
}

// Decodes one Paletted Container (a run of `numEntries` global-registry IDs,
// bit-packed per protocol 775's format) into the plain global ID for each
// entry. `indirectMaxBits` is the bits-per-entry cutoff above which the
// container uses the direct format (no palette array, entries ARE global
// IDs) instead of indirect (entries are local indices into a palette array
// of VarInt global IDs) - 8 for block states, 3 for biomes.
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
    // else: direct format

    // bitsPerEntry is a raw byte straight off the wire (0-255) - a value
    // above 64 is never legitimate and would make entriesPerLong below
    // truncate to 0, hard-crashing the process on an integer divide-by-zero
    // (a CPU trap, not a catchable C++ exception). Bail with a real
    // exception instead; NetworkClient's chunk handler logs and skips it.
    if (bitsPerEntry > 64) {
        throw std::runtime_error("Paletted container has an implausible bitsPerEntry ("
            + std::to_string(bitsPerEntry) + ") - packet is likely misaligned/misidentified.");
    }

    int entriesPerLong = 64 / bitsPerEntry;

    // The data array's length is NOT sent
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
    // array), not an NBT compound - no NBT parsing needed to skip past it.
    int32_t heightmapCount = reader.ReadVarInt();
    for (int32_t i = 0; i < heightmapCount; i++) {
        reader.ReadVarInt(); // heightmap type, unused
        int32_t longCount = reader.ReadVarInt();
        reader.Skip(static_cast<size_t>(longCount) * 8);
    }

    reader.ReadVarInt(); // Size - sanity bound only; sections are read directly below.

    auto chunk = std::make_unique<Chunk>(chunkX, chunkZ);

    // Assumes a standard overworld dimension: 24 sections of 16, covering
    // world Y -64..319 (Chunk's local Y 0..383).
    constexpr int SECTION_COUNT = CHUNK_SIZE_Y / 16;
    for (int section = 0; section < SECTION_COUNT; section++) {
        reader.ReadShort(); // non-air block count, unused
        reader.ReadShort(); // non-air fluid count, unused

        std::vector<int64_t> blockStates = ReadPalettedContainer(reader, 4096, 8);
        int sectionBaseY = section * 16;
        auto registryLock = BlockRegistry::ReadLock(); // Per section; see BlockRegistry::Reload().
        for (int i = 0; i < 4096; i++) {
            // x fastest, then z, then y slowest
            int x = i % 16;
            int z = (i / 16) % 16;
            int ylocal = i / 256;
            int32_t stateId = static_cast<int32_t>(blockStates[static_cast<size_t>(i)]);

            chunk->setBlock(x, sectionBaseY + ylocal, z, BlockRegistry::ResolveStateIdUnlocked(stateId));
        }

        ReadPalettedContainer(reader, 64, 3); // biomes - parsed to stay byte-aligned, discarded.
    }

    // Block Entities: not modeled by this client yet, but must still be read
    // off the wire so the light data that follows lines up correctly.
    int32_t blockEntityCount = reader.ReadVarInt();
    for (int32_t i = 0; i < blockEntityCount; i++) {
        uint8_t packedXZ = reader.ReadByte(); // X in the high nibble, Z in the low.
        int16_t y = reader.ReadShort();       // Absolute world Y.
        chunk->AddBlockEntity(packedXZ >> 4, y - WORLD_MIN_Y, packedXZ & 0x0F);
        reader.ReadVarInt(); // type, unused

        // Network NBT (no name field)
        auto tagType = static_cast<NBT::TagType>(reader.ReadByte());
        if (tagType != NBT::TagType::End) {
            std::string buffer(reinterpret_cast<const char*>(reader.Cursor()), reader.Remaining());
            std::istringstream stream(buffer, std::ios::binary);
            NBT::ReadPayload(stream, tagType);
            reader.Skip(static_cast<size_t>(stream.tellg()));
        }
    }

    // Light: 26 possible sections (24 real + one boundary section below and
    // above the chunk), each flagged present via a bitset mask rather than
    // sent unconditionally. Mask bit i covers section (i - 1) in Chunk's
    // local-Y section numbering (bit 0 is the sub-chunk below the world
    // floor, bit SECTION_COUNT+1 the one above the ceiling).
    std::vector<uint64_t> skyLightMask = ReadBitSet(reader);
    std::vector<uint64_t> blockLightMask = ReadBitSet(reader);
    std::vector<uint64_t> emptySkyLightMask = ReadBitSet(reader);
    std::vector<uint64_t> emptyBlockLightMask = ReadBitSet(reader);

    constexpr int LIGHT_SECTION_COUNT = SECTION_COUNT + 2;

    reader.ReadVarInt(); // Sky Light array count
    for (int bit = 0; bit < LIGHT_SECTION_COUNT; bit++) {
        if (!BitSetTest(skyLightMask, bit)) continue;
        std::vector<uint8_t> nibbles = ReadLightArray(reader);

        int section = bit - 1;
        if (section < 0 || section >= SECTION_COUNT) continue; // boundary section, no blocks to light
        int sectionBaseY = section * 16;
        for (int i = 0; i < 4096; i++) {
            uint8_t value = (i % 2 == 0) ? (nibbles[static_cast<size_t>(i / 2)] & 0x0Fu)
                                          : (nibbles[static_cast<size_t>(i / 2)] >> 4);
            chunk->setSkyLight(i % 16, sectionBaseY + i / 256, (i / 16) % 16, value);
        }
    }

    reader.ReadVarInt(); // Block Light array count
    for (int bit = 0; bit < LIGHT_SECTION_COUNT; bit++) {
        if (!BitSetTest(blockLightMask, bit)) continue;
        std::vector<uint8_t> nibbles = ReadLightArray(reader);

        int section = bit - 1;
        if (section < 0 || section >= SECTION_COUNT) continue;
        int sectionBaseY = section * 16;
        for (int i = 0; i < 4096; i++) {
            uint8_t value = (i % 2 == 0) ? (nibbles[static_cast<size_t>(i / 2)] & 0x0Fu)
                                          : (nibbles[static_cast<size_t>(i / 2)] >> 4);
            chunk->applyBlockLight(i % 16, sectionBaseY + i / 256, (i / 16) % 16, value);
        }
    }

    for (int bit = 0; bit < LIGHT_SECTION_COUNT; bit++) {
        int section = bit - 1;
        if (section < 0 || section >= SECTION_COUNT) continue;
        bool emptySky = BitSetTest(emptySkyLightMask, bit);
        bool emptyBlock = BitSetTest(emptyBlockLightMask, bit);
        if (!emptySky && !emptyBlock) continue;

        int sectionBaseY = section * 16;
        for (int i = 0; i < 4096; i++) {
            int x = i % 16;
            int z = (i / 16) % 16;
            int y = sectionBaseY + i / 256;
            if (emptySky) chunk->setSkyLight(x, y, z, 0);
            if (emptyBlock) chunk->setBlockLight(x, y, z, 0);
        }
    }

    return chunk;
}

PendingLightUpdate ParseLightUpdatePacket(PacketReader& reader)
{
    PendingLightUpdate update;
    update.chunkX = reader.ReadVarInt();
    update.chunkZ = reader.ReadVarInt();

    std::vector<uint64_t> skyMask = ReadBitSet(reader);
    std::vector<uint64_t> blockMask = ReadBitSet(reader);
    std::vector<uint64_t> emptySkyMask = ReadBitSet(reader);
    std::vector<uint64_t> emptyBlockMask = ReadBitSet(reader);

    constexpr int LIGHT_SECTION_COUNT = SECTIONS_PER_CHUNK + 2; // Bit 0 is the boundary section below the world.

    auto readChannel = [&](const std::vector<uint64_t>& mask, const std::vector<uint64_t>& emptyMask,
                           std::array<std::vector<uint8_t>, SECTIONS_PER_CHUNK>& out)
    {
        reader.ReadVarInt(); // Array count
        for (int bit = 0; bit < LIGHT_SECTION_COUNT; bit++)
        {
            int section = bit - 1;
            bool inRange = section >= 0 && section < SECTIONS_PER_CHUNK;
            if (BitSetTest(mask, bit))
            {
                std::vector<uint8_t> nibbles = ReadLightArray(reader);
                if (inRange) out[static_cast<size_t>(section)] = std::move(nibbles);
            }
            else if (inRange && BitSetTest(emptyMask, bit))
            {
                out[static_cast<size_t>(section)].assign(2048, 0);
            }
        }
    };

    readChannel(skyMask, emptySkyMask, update.skyLight);
    readChannel(blockMask, emptyBlockMask, update.blockLight);
    return update;
}

} // namespace Volcano::ChunkParser
