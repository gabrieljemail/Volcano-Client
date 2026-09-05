#include "SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include <cstring>
#include <iostream>

namespace Volcano {

void SlabBuffer::Init(VkDeviceSize capacityIn, VkBufferUsageFlags usage) {
    capacity = capacityIn;
    cursor = 0;

    buffer = CreateBuffer(capacity, usage, true /* isUMA: host-visible, sequential write */);
    vmaMapMemory(vmaAllocator, buffer.allocation, &mapped);
}

void SlabBuffer::Destroy() {
    if (buffer.buffer == VK_NULL_HANDLE) return;

    if (mapped) {
        vmaUnmapMemory(vmaAllocator, buffer.allocation);
        mapped = nullptr;
    }

    vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
    buffer = AllocatedBuffer{};
    capacity = 0;
    cursor = 0;
}

bool SlabBuffer::Allocate(const void* data, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& outOffset) {
    if (size == 0) {
        outOffset = cursor;
        return true;
    }

    // Division-based rounding: alignment (e.g. sizeof(PackedVertex) == 12) is
    // not always a power of two, so the classic `& ~(alignment - 1)` bitmask
    // trick doesn't round up correctly here — it silently produces offsets
    // that are neither a multiple of `alignment` nor even always divisible
    // by it, since that trick is only valid for power-of-two alignments.
    VkDeviceSize alignedOffset = alignment > 0
        ? ((cursor + alignment - 1) / alignment) * alignment
        : cursor;

    if (alignedOffset + size > capacity) {
        std::cerr << "[ERROR] SlabBuffer out of space (capacity " << capacity
                  << " bytes, requested " << size << " bytes at offset " << alignedOffset
                  << "). Consider increasing the slab size." << std::endl;
        return false;
    }

    memcpy(static_cast<uint8_t*>(mapped) + alignedOffset, data, (size_t)size);
    cursor = alignedOffset + size;
    outOffset = alignedOffset;
    return true;
}

} // namespace Volcano
