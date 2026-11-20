#pragma once
#ifndef SLAB_BUFFER_D3D11_H
#define SLAB_BUFFER_D3D11_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "SlabBuffer.hpp"

namespace Volcano {

// The Direct3D 11 SlabBuffer: dynamic ID3D11Buffers written through
// Map(MAP_WRITE_NO_OVERWRITE), which - like the Vulkan sibling's
// persistently-mapped blocks - lets the CPU append to a region while the GPU
// is still reading other regions of the same buffer. The delayed free list
// (FREE_DELAY_FRAMES) is what makes NO_OVERWRITE safe: a region is never
// rewritten until no in-flight draw can reference it.
//
// The device/context aren't owned here; D3D11RenderEngine::Init registers
// them once via SetDeviceContext() before any mesher runs.
class SlabBufferD3D11 final : public SlabBuffer {
public:
    static void SetDeviceContext(ID3D11Device* deviceIn, ID3D11DeviceContext* contextIn);

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
        ID3D11Buffer* buffer = nullptr;
        uint64_t capacity = 0;
        std::map<uint64_t, uint64_t> freeRanges; // offset -> size
    };
    struct PendingFree { ID3D11Buffer* buffer; uint64_t offset, size; uint64_t frame; };

    void ReclaimPendingLocked();
    static void InsertFreeLocked(Block& block, uint64_t offset, uint64_t size);
    bool AllocateFromBlockLocked(Block& block, const void* data, uint64_t size, uint64_t alignment, uint64_t& outOffset);
    Block* AddBlockLocked(uint64_t minimumSize);
    const char* growFailure = ""; // Why the last AddBlockLocked said no, for the log.
    void DestroyBlockLocked(Block& block);

    uint32_t usage = 0;
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
