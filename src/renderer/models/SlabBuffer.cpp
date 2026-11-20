#include "SlabBuffer.hpp"
#include "SlabBufferVulkan.hpp"
#include "SlabBufferD3D11.hpp"

namespace Volcano {

namespace {

SlabBufferBackend activeBackend = SlabBufferBackend::Vulkan;

} // namespace

void SetSlabBufferBackend(SlabBufferBackend backend)
{
    activeBackend = backend;
}

std::unique_ptr<SlabBuffer> CreateSlabBuffer()
{
    if (activeBackend == SlabBufferBackend::D3D11) return std::make_unique<SlabBufferD3D11>();
    return std::make_unique<SlabBufferVulkan>();
}

} // namespace Volcano
