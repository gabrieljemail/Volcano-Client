#pragma once
#ifndef CHUNK_H
#define CHUNK_H

#include <vector>
#include <array>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <unordered_set>
#include "Block.hpp"

namespace Volcano {

constexpr int CHUNK_SIZE_X = 16;
constexpr int CHUNK_SIZE_Y = 384;
constexpr int CHUNK_SIZE_Z = 16;

// World Y of the bottom of a chunk (local Y 0). Real overworld chunks span
// world Y -64..319 (24 sections of 16), stored here as local Y 0..383.
constexpr int WORLD_MIN_Y = -64;

// Sky and block light kept apart (rather than pre-merged into one value),
// so the renderer can tell "lit by open sky" from "lit by an actual light
// source" - see terrain.frag/misc.frag's own comment on why that split
// drives the bloom-style brightness cap. GetLightAcrossChunks in each mesher
// returns one of these per lookup instead of two separate calls, so a
// cross-chunk sample only pays for one World lookup, not two.
struct LightSample {
    uint8_t sky = 15;
    uint8_t block = 0;
};

// A column is 24 sections of 16x16x16. Each section stores a palette of the
// distinct Blocks it holds plus a 16-bit palette index per cell, and only
// once it holds something other than air. Light is 4 bits per cell and only
// stored once it differs from the default (sky 15, block 0). A mostly-empty
// column costs around a tenth of a flat array of Blocks.
class Chunk {
public:
    Chunk(int chunkX, int chunkZ) : x(chunkX), z(chunkZ) {}

    int getX() const { return x; }
    int getZ() const { return z; }
    void SetFadeStart(std::chrono::steady_clock::time_point time) { fadeStart = time; }
    std::chrono::steady_clock::time_point GetFadeStart() const { return fadeStart; }

    Block getBlock(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return Block{}; // Simple out-of-bounds check for now
        const Section& section = sections[by >> 4];
        if (section.cells.empty()) return Block{};
        return section.palette[section.cells[CellIndex(bx, by, bz)]];
    }

    // Rewrites every block through fn(Block) -> Block (resource reload remaps
    // visual ids). Caller provides any needed locking (see World). Only the
    // palettes are rewritten; sections still holding nothing stay air.
    template<typename Fn>
    void TransformBlocks(Fn&& fn) {
        for (Section& section : sections) {
            for (Block& block : section.palette) block = fn(block);
        }
    }

    void setBlock(int bx, int by, int bz, uint16_t visualId) {
        setBlock(bx, by, bz, Block{visualId});
    }

    // Full-block overload, for callers (ChunkParser) that also resolved a
    // non-cube visual/collision flag
    void setBlock(int bx, int by, int bz, const Block& block) {
        if (!InBounds(bx, by, bz)) return;
        Section& section = sections[by >> 4];
        if (section.cells.empty()) {
            if (block == Block{}) return;
            section.palette.assign(1, Block{});
            section.cells.assign(SECTION_CELLS, 0);
        }
        section.cells[CellIndex(bx, by, bz)] = PaletteIndex(section, block);
    }

    // True if every block in local Y range [sectionY*16, sectionY*16+16) is
    // air - lets MeshingThread skip greedy-sweeping/uploading a section
    // with nothing to draw (common near build-height limits) instead of
    // producing (and slab-allocating) an empty mesh for it every time.
    // sectionY out of [0, CHUNK_SIZE_Y/16) reads as empty, matching
    // getBlock's own out-of-bounds-is-Air convention.
    bool IsSectionEmpty(int sectionY) const {
        if (sectionY < 0 || sectionY >= SECTION_COUNT) return true;
        const Section& section = sections[sectionY];
        if (section.cells.empty()) return true;

        // Cheap exit when the palette itself is all air (after block breaks).
        bool anyBlock = false;
        for (const Block& block : section.palette) {
            if (!block.isAir()) { anyBlock = true; break; }
        }
        if (!anyBlock) return true;

        for (uint16_t index : section.cells) {
            if (!section.palette[index].isAir()) return false;
        }
        return true;
    }

    // Cells holding a block entity (chest, furnace, sign, ...), from chunk
    // data. Not updated by later Block Updates, so a broken chest leaves a
    // stale entry until the chunk reloads.
    void AddBlockEntity(int bx, int by, int bz) {
        if (InBounds(bx, by, bz)) blockEntities.insert(Index(bx, by, bz));
    }

    bool HasBlockEntity(int bx, int by, int bz) const {
        return InBounds(bx, by, bz) && blockEntities.count(Index(bx, by, bz)) != 0;
    }

    // 0-15 each, kept separate
    uint8_t getSkyLight(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return 15; // Out-of-bounds reads as full-bright, matching the default.
        const Section& section = sections[by >> 4];
        if (section.skyLight.empty()) return 15;
        return GetNibble(section.skyLight, CellIndex(bx, by, bz));
    }

    uint8_t getBlockLight(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return 0;
        const Section& section = sections[by >> 4];
        if (section.blockLight.empty()) return 0;
        return GetNibble(section.blockLight, CellIndex(bx, by, bz));
    }

    // Both channels in one call
    LightSample getLightSample(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return LightSample{};
        const Section& section = sections[by >> 4];
        size_t cell = CellIndex(bx, by, bz);
        return LightSample{
            section.skyLight.empty() ? uint8_t{15} : GetNibble(section.skyLight, cell),
            section.blockLight.empty() ? uint8_t{0} : GetNibble(section.blockLight, cell)
        };
    }

    // Sky light is set directly (ChunkParser applies it whole-section, so
    // there's nothing to combine with yet); block light is applied after
    // and only ever raises the value (a torch in an otherwise-dark cave).
    void setSkyLight(int bx, int by, int bz, uint8_t value) {
        if (!InBounds(bx, by, bz)) return;
        SetLight(sections[by >> 4].skyLight, CellIndex(bx, by, bz), value, 15);
    }

    // Overwrites (can lower)
    void setBlockLight(int bx, int by, int bz, uint8_t value) {
        if (!InBounds(bx, by, bz)) return;
        SetLight(sections[by >> 4].blockLight, CellIndex(bx, by, bz), value, 0);
    }

    void applyBlockLight(int bx, int by, int bz, uint8_t value) {
        if (!InBounds(bx, by, bz)) return;
        std::vector<uint8_t>& light = sections[by >> 4].blockLight;
        size_t cell = CellIndex(bx, by, bz);
        uint8_t current = light.empty() ? uint8_t{0} : GetNibble(light, cell);
        if (value > current) SetLight(light, cell, value, 0);
    }

    // Heap bytes held by block and light storage, for memory logging.
    size_t MemoryBytes() const {
        size_t bytes = 0;
        for (const Section& section : sections) {
            bytes += section.palette.capacity() * sizeof(Block);
            bytes += section.cells.capacity() * sizeof(uint16_t);
            bytes += section.skyLight.capacity() + section.blockLight.capacity();
        }
        return bytes;
    }

private:
    static constexpr int SECTION_COUNT = CHUNK_SIZE_Y / 16;
    static constexpr size_t SECTION_CELLS = 16 * 16 * 16;
    static constexpr size_t LIGHT_BYTES = SECTION_CELLS / 2;

    // Empty cells means the whole section is air; empty light means the
    // default for that channel.
    struct Section {
        std::vector<Block> palette;
        std::vector<uint16_t> cells;
        std::vector<uint8_t> skyLight;
        std::vector<uint8_t> blockLight;
        uint16_t lastIndex = 0; // Runs of one block hit this before the palette search.
    };

    int x, z; // Chunk coordinates
    std::chrono::steady_clock::time_point fadeStart{};
    std::array<Section, SECTION_COUNT> sections;
    std::unordered_set<size_t> blockEntities;

    static bool InBounds(int bx, int by, int bz) {
        return bx >= 0 && bx < CHUNK_SIZE_X && by >= 0 && by < CHUNK_SIZE_Y && bz >= 0 && bz < CHUNK_SIZE_Z;
    }

    // Column-wide index, used for the block entity set.
    static size_t Index(int bx, int by, int bz) {
        return static_cast<size_t>(bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z);
    }

    // Index inside one section.
    static size_t CellIndex(int bx, int by, int bz) {
        return static_cast<size_t>(bx + bz * CHUNK_SIZE_X + (by & 15) * CHUNK_SIZE_X * CHUNK_SIZE_Z);
    }

    static uint8_t GetNibble(const std::vector<uint8_t>& nibbles, size_t cell) {
        uint8_t packed = nibbles[cell >> 1];
        return (cell & 1) ? (packed >> 4) : (packed & 0x0F);
    }

    // Allocates the section's light on the first value that differs from the default.
    static void SetLight(std::vector<uint8_t>& nibbles, size_t cell, uint8_t value, uint8_t defaultValue) {
        value &= 0x0F;
        if (nibbles.empty()) {
            if (value == defaultValue) return;
            nibbles.assign(LIGHT_BYTES, static_cast<uint8_t>(defaultValue | (defaultValue << 4)));
        }
        uint8_t& packed = nibbles[cell >> 1];
        packed = (cell & 1) ? static_cast<uint8_t>((packed & 0x0F) | (value << 4))
                            : static_cast<uint8_t>((packed & 0xF0) | value);
    }

    static uint16_t PaletteIndex(Section& section, const Block& block) {
        if (block == section.palette[section.lastIndex]) return section.lastIndex;
        for (size_t i = 0; i < section.palette.size(); i++) {
            if (block == section.palette[i]) {
                section.lastIndex = static_cast<uint16_t>(i);
                return section.lastIndex;
            }
        }
        if (section.palette.size() >= 0xFFFF) CompactPalette(section);
        section.palette.push_back(block);
        section.lastIndex = static_cast<uint16_t>(section.palette.size() - 1);
        return section.lastIndex;
    }

    // Drops palette entries no cell uses (edits leave stale ones behind).
    static void CompactPalette(Section& section) {
        std::vector<Block> compact;
        std::vector<uint32_t> remap(section.palette.size(), UINT32_MAX);
        for (uint16_t& cell : section.cells) {
            uint32_t& mapped = remap[cell];
            if (mapped == UINT32_MAX) {
                mapped = static_cast<uint32_t>(compact.size());
                compact.push_back(section.palette[cell]);
            }
            cell = static_cast<uint16_t>(mapped);
        }
        section.palette = std::move(compact);
        section.lastIndex = 0;
    }
};

} // namespace Volcano

#endif
