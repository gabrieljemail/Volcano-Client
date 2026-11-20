#pragma once
#ifndef SLAB_BUFFER_VULKAN_H
#define SLAB_BUFFER_VULKAN_H

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
#include "SlabBuffer.hpp"

namespace Volcano {

// The Vulkan SlabBuffer: VMA-allocated, persistently host-mapped blocks.
class SlabBufferVulkan final : public SlabBuffer {
public:
    void Init(uint64_t limit, uint32_t usage) override;
    void Destroy() override;

    bool Allocate(const void* data, uint64_t size, uint64_t alignment,
        void*& outBuffer, uint64_t& outOffset) override;
    void Free(void* buffer, uint64_t offset, uint64_t size) override;
    bool Owns(void* buffer) const override;

    uint64_t GetCapacity() const override { return capacity.load(); }
    uint64_t GetLimit() const override { return limit; }
    uint64_t GetUsed() const override { return used.load(); }
    uint32_t GetBlockCount() const override { return blockCount.load(); }
    bool IsInitialized() const override { return initialized; }

private:
    struct Block {
        AllocatedBuffer buffer{};
        void* mapped = nullptr;
        uint64_t capacity = 0;
        std::map<uint64_t, uint64_t> freeRanges; // offset -> size
    };
    struct PendingFree { VkBuffer buffer; uint64_t offset, size; uint64_t frame; }; // buffer stored as VkBuffer; Free() casts the opaque handle in

    void ReclaimPendingLocked();
    static void InsertFreeLocked(Block& block, uint64_t offset, uint64_t size);
    bool AllocateFromBlockLocked(Block& block, const void* data, uint64_t size, uint64_t alignment, uint64_t& outOffset);
    Block* AddBlockLocked(uint64_t minimumSize);
    const char* growFailure = ""; // Why the last AddBlockLocked said no, for the log.
    void DestroyBlockLocked(Block& block);

    VkBufferUsageFlags usage = 0;
    uint64_t limit = 0;
    bool initialized = false;
    std::atomic<uint64_t> capacity{0};   // Sum of block sizes.
    std::atomic<uint64_t> used{0};       // Bytes currently handed out (excl. pending frees).
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
