#include "SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "Logger.hpp"
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>

namespace Volcano {

void SlabBuffer::Init(VkDeviceSize capacityIn, VkBufferUsageFlags usage) {
    std::lock_guard<std::mutex> lock(mutex);
    capacity = capacityIn;
    used = 0;
    freeRanges.clear();
    pendingFrees.clear();

    buffer = CreateBuffer(capacity, usage, true /* isUMA: host-visible, sequential write */);
    // CreateBuffer doesn't check vmaCreateBuffer's result itself, so a
    // request too large for the host-visible heap (a real risk here now that
    // this size scales with Graphics.RenderDistance — that heap is often
    // capped hard, e.g. ~256 MB on a GPU without resizable BAR) silently
    // leaves buffer.allocation null. Mapping that null allocation is what
    // used to crash with an opaque VMA assertion instead of a readable
    // error — catch it here instead.
    if (buffer.buffer == VK_NULL_HANDLE || buffer.allocation == nullptr) {
        throw std::runtime_error("[ERROR] SlabBuffer failed to allocate " + std::to_string(capacity)
            + " bytes — try lowering Graphics.RenderDistance.");
    }
    vmaMapMemory(vmaAllocator, buffer.allocation, &mapped);

    freeRanges[0] = capacity; // the whole slab starts out free
}

void SlabBuffer::Destroy() {
    std::lock_guard<std::mutex> lock(mutex);
    if (buffer.buffer == VK_NULL_HANDLE) return;

    if (mapped) {
        vmaUnmapMemory(vmaAllocator, buffer.allocation);
        mapped = nullptr;
    }

    vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
    buffer = AllocatedBuffer{};
    capacity = 0;
    used = 0;
    freeRanges.clear();
    pendingFrees.clear();
}

// Adds [offset, offset + size) to freeRanges, merging with any adjacent
// free range on either side so the list doesn't fragment into slivers.
void SlabBuffer::InsertFreeLocked(VkDeviceSize offset, VkDeviceSize size) {
    if (size == 0) return;

    auto next = freeRanges.lower_bound(offset);
    if (next != freeRanges.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == offset) {
            offset = prev->first;
            size += prev->second;
            freeRanges.erase(prev);
        }
    }
    if (next != freeRanges.end() && offset + size == next->first) {
        size += next->second;
        freeRanges.erase(next);
    }
    freeRanges[offset] = size;
}

void SlabBuffer::ReclaimPendingLocked() {
    uint64_t now = renderFrameCounter.load();
    while (!pendingFrees.empty() && now >= pendingFrees.front().frame + FREE_DELAY_FRAMES) {
        InsertFreeLocked(pendingFrees.front().offset, pendingFrees.front().size);
        pendingFrees.pop_front();
    }
}

bool SlabBuffer::Allocate(const void* data, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& outOffset) {
    std::lock_guard<std::mutex> lock(mutex);
    if (size == 0) {
        outOffset = 0;
        return true;
    }

    ReclaimPendingLocked();

    // First fit. Division-based rounding: alignment (e.g.
    // sizeof(PackedVertex) == 12) is not always a power of two, so the
    // classic `& ~(alignment - 1)` bitmask trick doesn't round correctly.
    for (auto it = freeRanges.begin(); it != freeRanges.end(); ++it) {
        VkDeviceSize start = it->first;
        VkDeviceSize end = start + it->second;
        VkDeviceSize aligned = alignment > 0 ? ((start + alignment - 1) / alignment) * alignment : start;
        if (aligned + size > end) continue;

        freeRanges.erase(it);
        // Alignment padding before, and the remainder after, stay free.
        InsertFreeLocked(start, aligned - start);
        InsertFreeLocked(aligned + size, end - (aligned + size));

        memcpy(static_cast<uint8_t*>(mapped) + aligned, data, static_cast<size_t>(size));
        used += size;
        outOffset = aligned;
        return true;
    }

    Log::Error("SlabBuffer out of space (capacity " + std::to_string(capacity)
              + " bytes, " + std::to_string(used.load()) + " in use, requested " + std::to_string(size)
              + " bytes). Consider increasing Graphics.SlabSize.");
    return false;
}

void SlabBuffer::Free(VkDeviceSize offset, VkDeviceSize size) {
    if (size == 0) return;
    std::lock_guard<std::mutex> lock(mutex);
    if (buffer.buffer == VK_NULL_HANDLE) return;

    pendingFrees.push_back({ offset, size, renderFrameCounter.load() });
    used -= size;
}

} // namespace Volcano
