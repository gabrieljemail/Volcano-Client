#include "SlabBufferD3D11.hpp"
#include "Logger.hpp"
#include "platform/OS.hpp"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>

namespace Volcano {

namespace {

ID3D11Device* slabDevice = nullptr;
ID3D11DeviceContext* slabContext = nullptr;

// Growth stops while this much (plus the new block) wouldn't stay free in
// system RAM, so a big world can't push a low-RAM machine into swapping.
constexpr uint64_t RAM_HEADROOM_BYTES = 256ull * 1024 * 1024;

} // namespace

void SlabBufferD3D11::SetDeviceContext(ID3D11Device* deviceIn, ID3D11DeviceContext* contextIn)
{
    slabDevice = deviceIn;
    slabContext = contextIn;
}

void SlabBufferD3D11::Init(uint64_t limitIn, uint32_t usageIn)
{
    std::lock_guard<std::mutex> lock(mutex);
    usage = usageIn;
    limit = std::max(limitIn, BLOCK_SIZE);
    initialized = true;
}

void SlabBufferD3D11::Destroy()
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

void SlabBufferD3D11::DestroyBlockLocked(Block& block)
{
    if (block.buffer == nullptr) return;
    block.buffer->Release();
    block.buffer = nullptr;
}

// Adds [offset, offset + size) to the block's free ranges, merging with any
// adjacent free range on either side so the list doesn't fragment into slivers.
void SlabBufferD3D11::InsertFreeLocked(Block& block, uint64_t offset, uint64_t size)
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

void SlabBufferD3D11::ReclaimPendingLocked()
{
    uint64_t now = renderFrameCounter.load();
    while (!pendingFrees.empty() && now >= pendingFrees.front().frame + FREE_DELAY_FRAMES) {
        const PendingFree& pending = pendingFrees.front();
        for (size_t i = 0; i < blocks.size(); i++) {
            Block& block = *blocks[i];
            if (block.buffer != pending.buffer) continue;

            InsertFreeLocked(block, pending.offset, pending.size);

            // A block that's entirely free again is handed back (the first one
            // stays, so a steady-state world doesn't churn buffers).
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

bool SlabBufferD3D11::AllocateFromBlockLocked(Block& block, const void* data, uint64_t size, uint64_t alignment, uint64_t& outOffset)
{
    // First fit. Division-based rounding: alignment (e.g.
    // sizeof(PackedVertex) == 12) is not always a power of two, so the
    // classic `& ~(alignment - 1)` bitmask trick doesn't round correctly.
    for (auto it = block.freeRanges.begin(); it != block.freeRanges.end(); ++it) {
        uint64_t start = it->first;
        uint64_t rangeSize = it->second;
        uint64_t end = start + rangeSize;
        uint64_t aligned = alignment > 0 ? ((start + alignment - 1) / alignment) * alignment : start;
        if (aligned + size > end) continue;

        block.freeRanges.erase(it);
        // Alignment padding before, and the remainder after, stay free.
        InsertFreeLocked(block, start, aligned - start);
        InsertFreeLocked(block, aligned + size, end - (aligned + size));

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(slabContext->Map(block.buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped))) {
            // Put the whole range back so a transient map failure doesn't leak it.
            InsertFreeLocked(block, start, rangeSize);
            continue;
        }
        memcpy(static_cast<uint8_t*>(mapped.pData) + aligned, data, static_cast<size_t>(size));
        slabContext->Unmap(block.buffer, 0);
        outOffset = aligned;
        return true;
    }
    return false;
}

SlabBufferD3D11::Block* SlabBufferD3D11::AddBlockLocked(uint64_t minimumSize)
{
    uint64_t size = std::max(BLOCK_SIZE, minimumSize);
    growFailure = "the slab's size limit was reached";
    if (capacity.load() + size > limit) return nullptr;

    uint64_t available = Platform::OS::GetAvailablePhysicalMemoryBytes(); // 0 = unknown.
    growFailure = "free system RAM is below the headroom the slab keeps (see RAM_HEADROOM_BYTES)";
    if (available != 0 && available < size + RAM_HEADROOM_BYTES) return nullptr;
    growFailure = "the D3D11 buffer creation failed";
    if (slabDevice == nullptr || slabContext == nullptr) return nullptr;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(size);
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = 0;
    if (usage & SlabUsageVertex) desc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
    if (usage & SlabUsageIndex) desc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.MiscFlags = 0;

    auto block = std::make_unique<Block>();
    if (FAILED(slabDevice->CreateBuffer(&desc, nullptr, &block->buffer))) return nullptr;

    block->capacity = size;
    block->freeRanges[0] = size;
    capacity += size;
    blockCount++;
    blocks.push_back(std::move(block));
    return blocks.back().get();
}

bool SlabBufferD3D11::Allocate(const void* data, uint64_t size, uint64_t alignment, void*& outBuffer, uint64_t& outOffset)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (size == 0) {
        outBuffer = nullptr;
        outOffset = 0;
        return true;
    }

    ReclaimPendingLocked();

    uint64_t offset = 0;
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
                + " failed since the last report) because " + growFailure + " - those sections will retry.");
            lastFailureLog = now;
            suppressedFailures = 0;
        }
        return false;
    }

    used += size;
    outBuffer = target->buffer;
    outOffset = offset;
    return true;
}

void SlabBufferD3D11::Free(void* buffer, uint64_t offset, uint64_t size)
{
    if (size == 0 || buffer == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex);

    pendingFrees.push_back({ static_cast<ID3D11Buffer*>(buffer), offset, size, renderFrameCounter.load() });
    used -= size;
}

bool SlabBufferD3D11::Owns(void* buffer) const
{
    if (buffer == nullptr) return false;
    const ID3D11Buffer* d3dBuffer = static_cast<const ID3D11Buffer*>(buffer);
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& block : blocks) {
        if (block->buffer == d3dBuffer) return true;
    }
    return false;
}

} // namespace Volcano
