#pragma once
#ifndef SLAB_BUFFER_H
#define SLAB_BUFFER_H

#include <atomic>
#include <cstdint>
#include <memory>

namespace Volcano {

// Combined figures across a mesher's own slabs, for the debug HUD (see
// ChunkMesher::GetSlabStats/NonCubicMesher::GetSlabStats).
struct SlabStats {
    uint64_t used = 0;     // Bytes handed out to live meshes.
    uint64_t capacity = 0; // Bytes currently committed (all blocks).
    uint64_t limit = 0;    // The most the slabs may grow to.
    uint32_t blocks = 0;   // GPU buffers currently backing them.
};

// Incremented by the render engine at the start of every frame it records
// (after that frame slot's fence wait). SlabBuffer::Free stamps freed
// regions with it, and only reuses them once enough frames have started
// that no in-flight command buffer can still be reading them.
inline std::atomic<uint64_t> renderFrameCounter{0};

// Which GPU API CreateSlabBuffer() hands out. Set once by the render
// engine's Init() before any mesher runs; Vulkan is the default so
// engine-less builds (e.g. NetDiag) keep the historical behavior.
enum class SlabBufferBackend { Vulkan, D3D11 };
void SetSlabBufferBackend(SlabBufferBackend backend);

// Backend-neutral usage bits for SlabBuffer::Init (each implementation maps
// them onto its own bind/usage flags).
enum SlabBufferUsage : uint32_t {
    SlabUsageNone = 0,
    SlabUsageVertex = 1u << 0,
    SlabUsageIndex = 1u << 1,
};

// Persistently CPU-writable buffers that sub-allocations are carved out of,
// instead of creating a separate buffer/allocation per chunk mesh. This is
// the backend-neutral interface; SlabBufferVulkan (VMA + VkBuffer) and
// SlabBufferD3D11 (dynamic ID3D11Buffer + MAP_WRITE_NO_OVERWRITE) are the
// implementations, picked by CreateSlabBuffer().
//
// Grows on demand: it starts with nothing, adds a block (BLOCK_SIZE, or larger
// for one huge request) whenever no existing block has room, and hands a block
// back once it is completely free again. The total is capped by `limit`, and
// growth also stops while the system is low on free RAM. A mesh records
// which block's buffer it lives in, so a list of meshes may span several
// buffers - drawers must bind per buffer (see ChunkDrawBatch).
//
// Freed regions go on a free list (first-fit, adjacent ranges coalesced)
// after a short delay
class SlabBuffer {
public:
    // Frames a freed region waits before reuse: the render engine keeps at
    // most 2 frames in flight, +1 for the frame that may be mid-recording
    // with the old mesh when it's freed.
    static constexpr uint64_t FREE_DELAY_FRAMES = 3;

    // Size of one block. A request bigger than this gets a block of its own size.
    // Small, because every slab keeps one block for good: with six slabs, 16 MB blocks pinned 96 MB of RAM even in an empty world.
    static constexpr uint64_t BLOCK_SIZE = 4ull * 1024 * 1024;

    virtual ~SlabBuffer() = default;

    // Sets the cap on total committed bytes and the usage bits every block is
    // created with (SlabUsageVertex/SlabUsageIndex). Allocates nothing yet.
    virtual void Init(uint64_t limit, uint32_t usage) = 0;
    virtual void Destroy() = 0;

    // Copies `size` bytes from `data` into the slab, honoring `alignment` for
    // the resulting offset. Returns true and sets `outBuffer`/`outOffset` on
    // success; returns false if no block has room and it can't grow (limit
    // reached, or the system is low on RAM). `outBuffer` is this backend's
    // opaque buffer handle so Mesh can stay API-neutral.
    virtual bool Allocate(const void* data, uint64_t size, uint64_t alignment,
        void*& outBuffer, uint64_t& outOffset) = 0;

    // Returns [offset, offset + size) of `buffer` - exactly what Allocate
    // handed out
    virtual void Free(void* buffer, uint64_t offset, uint64_t size) = 0;

    // Whether `buffer` is one of this slab's blocks.
    virtual bool Owns(void* buffer) const = 0;

    virtual uint64_t GetCapacity() const = 0;
    virtual uint64_t GetLimit() const = 0;
    virtual uint64_t GetUsed() const = 0;
    virtual uint32_t GetBlockCount() const = 0;
    virtual bool IsInitialized() const = 0;
};

// Creates a slab for the backend last passed to SetSlabBufferBackend().
std::unique_ptr<SlabBuffer> CreateSlabBuffer();

} // namespace Volcano

#endif
