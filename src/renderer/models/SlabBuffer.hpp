#pragma once
#ifndef SLAB_BUFFER_H
#define SLAB_BUFFER_H

#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include "AllocatedBuffer.hpp"

namespace Volcano {

// Combined used/capacity across a mesher's own slab buffers, for the debug
// HUD (see ChunkMesher::GetSlabStats/NonCubicMesher::GetSlabStats).
struct SlabStats {
    uint64_t used = 0;
    uint64_t capacity = 0;
};

// Incremented by the render engine at the start of every frame it records
// (after that frame slot's fence wait). SlabBuffer::Free stamps freed
// regions with it, and only reuses them once enough frames have started
// that no in-flight command buffer can still be reading them.
inline std::atomic<uint64_t> renderFrameCounter{0};

// A large, persistently-mapped VkBuffer that sub-allocations are carved out
// of, instead of creating a separate VkBuffer/VmaAllocation per chunk mesh.
//
// Freed regions go on a free list (first-fit, adjacent ranges coalesced)
// after a short delay — see FREE_DELAY_FRAMES — rather than being leaked
// forever, which is what used to fill the slab up from remeshing alone.
// Thread-safe: MeshingThread allocates/frees, the HUD reads stats.
class SlabBuffer {
public:
    // Frames a freed region waits before reuse: the render engine keeps at
    // most 2 frames in flight, +1 for the frame that may be mid-recording
    // with the old mesh when it's freed.
    static constexpr uint64_t FREE_DELAY_FRAMES = 3;

    // Allocates the backing buffer. `usage` should include every usage bit the
    // buffer will ever be bound with (e.g. vertex and/or index buffer bits).
    void Init(VkDeviceSize capacity, VkBufferUsageFlags usage);

    // Caps one slab's size so all slabs together stay within a share of the
    // RAM currently free: slabs are host-visible, i.e. real system memory.
    // `share` is this mesher's fraction of the total slab budget (cubic and
    // non-cubic shares sum to 1); `slabCount` is how many slabs split it.
    static VkDeviceSize AffordableSize(VkDeviceSize wanted, double share, uint32_t slabCount);
    void Destroy();

    // Copies `size` bytes from `data` into the slab, honoring `alignment` for
    // the resulting offset. Returns true and sets `outOffset` on success;
    // returns false if no free region is large enough.
    bool Allocate(const void* data, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& outOffset);

    // Returns [offset, offset + size) — exactly what Allocate handed out —
    // to the slab, reusable after FREE_DELAY_FRAMES more frames.
    void Free(VkDeviceSize offset, VkDeviceSize size);

    VkBuffer GetBuffer() const { return buffer.buffer; }
    VkDeviceSize GetCapacity() const { return capacity; }
    VkDeviceSize GetUsed() const { return used.load(); }
    bool IsInitialized() const { return buffer.buffer != VK_NULL_HANDLE; }

private:
    struct PendingFree { VkDeviceSize offset, size; uint64_t frame; };

    void ReclaimPendingLocked();
    void InsertFreeLocked(VkDeviceSize offset, VkDeviceSize size);

    AllocatedBuffer buffer{};
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    std::atomic<VkDeviceSize> used{0}; // bytes currently handed out (incl. pending frees)

    std::mutex mutex;
    std::map<VkDeviceSize, VkDeviceSize> freeRanges; // offset -> size
    std::deque<PendingFree> pendingFrees;            // oldest first
};

} // namespace Volcano

#endif
