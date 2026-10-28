#include "SlabBuffer.hpp"
#include "../VulkanInit.hpp"
#include "Logger.hpp"
#include "platform/OS.hpp"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

namespace Volcano {

namespace {

// Growth stops while this much (plus the new block) wouldn't stay free in
// system RAM, so a big world can't push a low-RAM machine into swapping.
constexpr uint64_t RAM_HEADROOM_BYTES = 256ull * 1024 * 1024;

} // namespace

void SlabBuffer::Init(VkDeviceSize limitIn, VkBufferUsageFlags usageIn)
{
    std::lock_guard<std::mutex> lock(mutex);
    usage = usageIn;
    limit = std::max(limitIn, BLOCK_SIZE);
    initialized = true;
}

void SlabBuffer::Destroy()
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& block : blocks) DestroyBlockLocked(*block);
    blocks.clear();
    pendingFrees.clear();
    capacity = 0;
    used = 0;
    blockCount = 0;
    initialized = false;
}

void SlabBuffer::DestroyBlockLocked(Block& block)
{
    if (block.buffer.buffer == VK_NULL_HANDLE) return;
    if (block.mapped) vmaUnmapMemory(vmaAllocator, block.buffer.allocation);
    vmaDestroyBuffer(vmaAllocator, block.buffer.buffer, block.buffer.allocation);
    block.buffer = AllocatedBuffer{};
    block.mapped = nullptr;
}

// Adds [offset, offset + size) to the block's free ranges, merging with any
// adjacent free range on either side so the list doesn't fragment into slivers.
void SlabBuffer::InsertFreeLocked(Block& block, VkDeviceSize offset, VkDeviceSize size)
{
    if (size == 0) return;

    auto& freeRanges = block.freeRanges;
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

void SlabBuffer::ReclaimPendingLocked()
{
    uint64_t now = renderFrameCounter.load();
    while (!pendingFrees.empty() && now >= pendingFrees.front().frame + FREE_DELAY_FRAMES) {
        const PendingFree& pending = pendingFrees.front();
        for (size_t i = 0; i < blocks.size(); i++) {
            Block& block = *blocks[i];
            if (block.buffer.buffer != pending.buffer) continue;

            InsertFreeLocked(block, pending.offset, pending.size);

            // A block that's entirely free again is handed back (the first one
            // stays, so a steady-state world doesn't churn VkBuffers).
            if (i > 0 && block.freeRanges.size() == 1 && block.freeRanges.begin()->first == 0
                && block.freeRanges.begin()->second == block.capacity) {
                capacity -= block.capacity;
                blockCount--;
                DestroyBlockLocked(block);
                blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
            }
            break;
        }
        pendingFrees.pop_front();
    }
}

bool SlabBuffer::AllocateFromBlockLocked(Block& block, const void* data, VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& outOffset)
{
    // First fit. Division-based rounding: alignment (e.g.
    // sizeof(PackedVertex) == 12) is not always a power of two, so the
    // classic `& ~(alignment - 1)` bitmask trick doesn't round correctly.
    for (auto it = block.freeRanges.begin(); it != block.freeRanges.end(); ++it) {
        VkDeviceSize start = it->first;
        VkDeviceSize end = start + it->second;
        VkDeviceSize aligned = alignment > 0 ? ((start + alignment - 1) / alignment) * alignment : start;
        if (aligned + size > end) continue;

        block.freeRanges.erase(it);
        // Alignment padding before, and the remainder after, stay free.
        InsertFreeLocked(block, start, aligned - start);
        InsertFreeLocked(block, aligned + size, end - (aligned + size));

        memcpy(static_cast<uint8_t*>(block.mapped) + aligned, data, static_cast<size_t>(size));
        outOffset = aligned;
        return true;
    }
    return false;
}

SlabBuffer::Block* SlabBuffer::AddBlockLocked(VkDeviceSize minimumSize)
{
    VkDeviceSize size = std::max(BLOCK_SIZE, minimumSize);
    if (capacity.load() + size > limit) return nullptr;

    uint64_t available = Platform::OS::GetAvailablePhysicalMemoryBytes(); // 0 = unknown.
    if (available != 0 && available < size + RAM_HEADROOM_BYTES) return nullptr;

    auto block = std::make_unique<Block>();
    block->buffer = CreateBuffer(size, usage, true /* isUMA: host-visible, sequential write */);
    if (block->buffer.buffer == VK_NULL_HANDLE || block->buffer.allocation == nullptr) return nullptr;

    vmaMapMemory(vmaAllocator, block->buffer.allocation, &block->mapped);
    block->capacity = size;
    block->freeRanges[0] = size;
    capacity += size;
    blockCount++;
    blocks.push_back(std::move(block));
    return blocks.back().get();
}

bool SlabBuffer::Allocate(const void* data, VkDeviceSize size, VkDeviceSize alignment, VkBuffer& outBuffer, VkDeviceSize& outOffset)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (size == 0) {
        outBuffer = VK_NULL_HANDLE;
        outOffset = 0;
        return true;
    }

    ReclaimPendingLocked();

    VkDeviceSize offset = 0;
    Block* target = nullptr;
    for (auto& block : blocks) {
        if (AllocateFromBlockLocked(*block, data, size, alignment, offset)) {
            target = block.get();
            break;
        }
    }

    if (target == nullptr) {
        // Room for the request even after worst-case alignment padding.
        Block* grown = AddBlockLocked(size + alignment);
        if (grown != nullptr && AllocateFromBlockLocked(*grown, data, size, alignment, offset)) target = grown;
    }

    if (target == nullptr) {
        suppressedFailures++;
        auto now = std::chrono::steady_clock::now();
        if (now - lastFailureLog >= std::chrono::seconds(2)) {
            Log::Error("SlabBuffer can't grow (" + std::to_string(capacity.load() / (1024 * 1024)) + " MB committed of a "
                + std::to_string(limit / (1024 * 1024)) + " MB limit, " + std::to_string(used.load() / (1024 * 1024))
                + " MB in use, requested " + std::to_string(size) + " bytes; " + std::to_string(suppressedFailures)
                + " failed since the last report) - those sections will retry. Raise Graphics.SlabSize or free RAM.");
            lastFailureLog = now;
            suppressedFailures = 0;
        }
        return false;
    }

    used += size;
    outBuffer = target->buffer.buffer;
    outOffset = offset;
    return true;
}

void SlabBuffer::Free(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size)
{
    if (size == 0 || buffer == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> lock(mutex);

    pendingFrees.push_back({ buffer, offset, size, renderFrameCounter.load() });
    used -= size;
}

bool SlabBuffer::Owns(VkBuffer buffer) const
{
    if (buffer == VK_NULL_HANDLE) return false;
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& block : blocks) {
        if (block->buffer.buffer == buffer) return true;
    }
    return false;
}

} // namespace Volcano
