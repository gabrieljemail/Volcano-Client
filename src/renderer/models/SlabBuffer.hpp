#pragma once
#ifndef SLAB_BUFFER_H
#define SLAB_BUFFER_H

#include <vulkan/vulkan.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "AllocatedBuffer.hpp"

namespace Volcano {

// Combined figures across a mesher's own slabs, for the debug HUD (see
// ChunkMesher::GetSlabStats/NonCubicMesher::GetSlabStats).
struct SlabStats {
    uint64_t used = 0;     // Bytes handed out to live meshes.
    uint64_t capacity = 0; // Bytes currently committed (all blocks).
    uint64_t limit = 0;    // The most the slabs may grow to.
    uint32_t blocks = 0;   // VkBuffers currently backing them.
};

// Incremented by the render engine at the start of every frame it records
// (after that frame slot's fence wait). SlabBuffer::Free stamps freed
// regions with it, and only reuses them once enough frames have started
// that no in-flight command buffer can still be reading them.
inline std::atomic<uint64_t> renderFrameCounter{0};

// Persistently-mapped VkBuffers that sub-allocations are carved out of,
// instead of creating a separate VkBuffer/VmaAllocation per chunk mesh.
//
// Grows on demand: it starts with nothing, adds a block (BLOCK_SIZE, or larger
// for one huge request) whenever no existing block has room, and hands a block
// back once it is completely free again. The total is capped by `limit`, and
// growth also stops while the system is low on free RAM (blocks are
// host-visible, i.e. real system memory on an integrated GPU). A mesh records
// which block's VkBuffer it lives in, so a list of meshes may span several
// buffers - drawers must bind per buffer (see ChunkDrawBatch).
//
// Freed regions go on a free list (first-fit, adjacent ranges coalesced)
// after a short delay - see FREE_DELAY_FRAMES - rather than being leaked
// forever. Thread-safe: MeshingThread allocates/frees, the HUD reads stats.
class SlabBuffer {
public:
    // Frames a freed region waits before reuse: the render engine keeps at
    // most 2 frames in flight, +1 for the frame that may be mid-recording
    // with the old mesh when it's freed.
    static constexpr uint64_t FREE_DELAY_FRAMES = 3;

    // Size of one block. A request bigger than this gets a block of its own size.
    static constexpr VkDeviceSize BLOCK_SIZE = 16ull * 1024 * 1024;

    // Sets the cap on total committed bytes and the usage bits every block is
    // created with (e.g. vertex and/or index buffer bits). Allocates nothing yet.
    void Init(VkDeviceSize limit, VkBufferUsageFlags usage);
    void Destroy();

    // Copies `size` bytes from `data` into the slab, honoring `alignment` for
    // the resulting offset. Returns true and sets `outBuffer`/`outOffset` on
    // success; returns false if no block has room and it can't grow (limit
    // reached, or the system is low on RAM).
    bool Allocate(const void* data, VkDeviceSize size, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset);

    // Returns [offset, offset + size) of `buffer` - exactly what Allocate
    // handed out - to the slab, reusable after FREE_DELAY_FRAMES more frames.
    void Free(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size);

    // Whether `buffer` is one of this slab's blocks.
    bool Owns(VkBuffer buffer) const;

    VkDeviceSize GetCapacity() const { return capacity.load(); }
    VkDeviceSize GetLimit() const { return limit; }
    VkDeviceSize GetUsed() const { return used.load(); }
    uint32_t GetBlockCount() const { return blockCount.load(); }
    bool IsInitialized() const { return initialized; }

private:
    struct Block {
        AllocatedBuffer buffer{};
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        std::map<VkDeviceSize, VkDeviceSize> freeRanges; // offset -> size
    };
    struct PendingFree { VkBuffer buffer; VkDeviceSize offset, size; uint64_t frame; };

    void ReclaimPendingLocked();
    static void InsertFreeLocked(Block& block, VkDeviceSize offset, VkDeviceSize size);
    bool AllocateFromBlockLocked(Block& block, const void* data, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& outOffset);
    Block* AddBlockLocked(VkDeviceSize minimumSize);
    void DestroyBlockLocked(Block& block);

    VkBufferUsageFlags usage = 0;
    VkDeviceSize limit = 0;
    bool initialized = false;
    std::atomic<VkDeviceSize> capacity{0};   // Sum of block sizes.
    std::atomic<VkDeviceSize> used{0};       // Bytes currently handed out (excl. pending frees).
    std::atomic<uint32_t> blockCount{0};

    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Block>> blocks;
    std::deque<PendingFree> pendingFrees;    // Oldest first.

    // Allocation failures can come in bursts (an explosion remeshing dozens of
    // sections); log at most one line every couple of seconds, with a count.
    std::chrono::steady_clock::time_point lastFailureLog{};
    uint32_t suppressedFailures = 0;
};

} // namespace Volcano

#endif
