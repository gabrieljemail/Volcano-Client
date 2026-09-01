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

    VkDeviceSize alignedOffset = alignment > 0
        ? (cursor + alignment - 1) & ~(alignment - 1)
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
