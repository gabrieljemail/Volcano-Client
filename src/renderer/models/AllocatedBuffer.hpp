#pragma once
#ifndef ALLOCATED_BUFFER_H
#define ALLOCATED_BUFFER_H

#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>

namespace Volcano {

struct AllocatedBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info{};
};

} // namespace Volcano

#endif
