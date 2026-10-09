#include "SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "Logger.hpp"
#include "platform/OS.hpp"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>

namespace Volcano {

VkDeviceSize SlabBuffer::AffordableSize(VkDeviceSize wanted, double share, uint32_t slabCount)
{
    // Never plan to use more than 40% of what's free; 0 means the query failed.
    constexpr double RAM_FRACTION = 0.4;
    constexpr VkDeviceSize MIN_SLAB = 8ull * 1024 * 1024;

    uint64_t available = Platform::OS::GetAvailablePhysicalMemoryBytes();
    if (available == 0) return wanted;

    double perSlab = static_cast<double>(available) * RAM_FRACTION * share / std::max(1u, slabCount);
    VkDeviceSize capped = std::max<VkDeviceSize>(MIN_SLAB, std::min<VkDeviceSize>(wanted, static_cast<VkDeviceSize>(perSlab)));
    if (capped < wanted) {
        Log::Info("SlabBuffer: capping a " + std::to_string(wanted / (1024 * 1024)) + " MB slab to "
            + std::to_string(capped / (1024 * 1024)) + " MB (" + std::to_string(available / (1024 * 1024))
            + " MB RAM free).");
    }
    return capped;
}

void SlabBuffer::Init(VkDeviceSize capacityIn, VkBufferUsageFlags usage) {
    std::lock_guard<std::mutex> lock(mutex);
    used = 0;
    freeRanges.clear();
    pendingFrees.clear();

    // CreateBuffer doesn't report failure except through a null allocation.
    // The host-visible heap can be small (low RAM, or capped without
    // resizable BAR), so halve the request until it fits rather than failing.
    constexpr VkDeviceSize MIN_CAPACITY = 8ull * 1024 * 1024;
    capacity = capacityIn;
    while (true) {
        buffer = CreateBuffer(capacity, usage, true /* isUMA: host-visible, sequential write */);
        if (buffer.buffer != VK_NULL_HANDLE && buffer.allocation != nullptr) break;

        if (capacity <= MIN_CAPACITY) {
            throw std::runtime_error("[ERROR] SlabBuffer couldn't allocate even " + std::to_string(MIN_CAPACITY)
                + " bytes — the system is out of memory.");
        }
        capacity /= 2;
    }
    if (capacity != capacityIn) {
        Log::Error("SlabBuffer: " + std::to_string(capacityIn / (1024 * 1024)) + " MB didn't fit; using "
            + std::to_string(capacity / (1024 * 1024)) + " MB. Lower Graphics.SlabSize.* to silence this.");
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
