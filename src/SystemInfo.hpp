#pragma once
#ifndef VOLCANO_SYSTEM_INFO_H
#define VOLCANO_SYSTEM_INFO_H

#include <cstdint>
#include <string>

namespace Volcano {

// Hardware/driver info for the debug overlay's "Key | Value" block (CPU,
// GPU, Display, Graphics API). Detected once - DetectCPU() at startup
// (before Vulkan exists), DetectGPU() right after VulkanInit picks a
// physical device
struct SystemInfo {
    std::string cpuModel;          // e.g. "Intel(R) Core(TM) i5-6600 CPU @ 3.30GHz"
    uint32_t cpuLogicalCores = 0;

    std::string gpuModel;          // VkPhysicalDeviceProperties::deviceName.
    std::string gpuDriverVendor;   // Decoded from VkPhysicalDeviceProperties::vendorID (Vulkan's own PCI vendor ID registry).
    uint32_t vulkanApiVersion = 0; // Packed VK_API_VERSION_MAJOR/MINOR/PATCH - decode with those macros, not by hand.

    // Fills cpuModel/cpuLogicalCores. Call once, before anything reads
    // them - no dependency on Vulkan/GLFW, safe to call first thing in
    // main(). Windows/x86 only (CPUID); cpuModel falls back to a plain
    // placeholder string on any other target.
    void DetectCPU();

    // Fills gpuModel/gpuDriverVendor/vulkanApiVersion from an already-
    // selected VkPhysicalDeviceProperties. Call once, right after
    // VulkanInit picks a physical device.
    void DetectGPU(const char* deviceName, uint32_t vendorID, uint32_t apiVersion);
};

// Resident RAM of this process, for the debug HUD. Live, not cached.
// Forwards to Platform::OS.
uint64_t GetProcessMemoryUsageBytes();

} // namespace Volcano

#endif
