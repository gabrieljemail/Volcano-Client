#pragma once
#ifndef SLAB_BUFFER_H
#define SLAB_BUFFER_H

#include <vulkan/vulkan.hpp>
#include "AllocatedBuffer.hpp"

namespace Volcano {

// A large, persistently-mapped VkBuffer that sub-allocations are bump-allocated
// from, instead of creating a separate VkBuffer/VmaAllocation per call.
//
// This avoids the overhead (and driver/allocator pressure) of allocating one
// tiny buffer per chunk mesh. Sub-allocations are never individually freed;
// the whole slab is released at once via Destroy().
class SlabBuffer {
public:
    // Allocates the backing buffer. `usage` should include every usage bit the
    // buffer will ever be bound with (e.g. vertex and/or index buffer bits).
    void Init(VkDeviceSize capacity, VkBufferUsageFlags usage);
    void Destroy();

    // Copies `size` bytes from `data` into the slab, honoring `alignment` for
    // the resulting offset. Returns true and sets `outOffset` on success;
    // returns false if the slab doesn't have enough room left.
    bool Allocate(const void* data, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& outOffset);

    VkBuffer GetBuffer() const { return buffer.buffer; }
    VkDeviceSize GetCapacity() const { return capacity; }
    VkDeviceSize GetUsed() const { return cursor; }
    bool IsInitialized() const { return buffer.buffer != VK_NULL_HANDLE; }

private:
    AllocatedBuffer buffer{};
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    VkDeviceSize cursor = 0;
};

} // namespace Volcano

#endif
