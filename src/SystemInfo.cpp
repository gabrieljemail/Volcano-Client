#include "SystemInfo.hpp"
#include "platform/OS.hpp"
#include <thread>

namespace Volcano {

void SystemInfo::DetectCPU()
{
    cpuLogicalCores = std::thread::hardware_concurrency();

    // Brand strings are often NUL- and space-padded.
    cpuModel = Platform::OS::GetCpuModel();
    size_t nul = cpuModel.find('\0');
    if (nul != std::string::npos) cpuModel.resize(nul);
    while (!cpuModel.empty() && cpuModel.back() == ' ') cpuModel.pop_back();
    size_t firstNonSpace = cpuModel.find_first_not_of(' ');
    cpuModel = (firstNonSpace == std::string::npos) ? "" : cpuModel.substr(firstNonSpace);

    if (cpuModel.empty()) cpuModel = "Unknown CPU";
}

void SystemInfo::DetectGPU(const char* deviceName, uint32_t vendorID, uint32_t apiVersion)
{
    gpuModel = (deviceName != nullptr && deviceName[0] != '\0') ? deviceName : "Unknown GPU";
    vulkanApiVersion = apiVersion;

    // Vulkan's own PCI vendor ID registry (the same field GPU-Z/vulkaninfo
    // decode the same way) — only the vendors this client is realistically
    // built/tested against get a friendly name; anything else still shows
    // something rather than silently guessing.
    switch (vendorID)
    {
        case 0x8086: gpuDriverVendor = "Intel"; break;
        case 0x10DE: gpuDriverVendor = "NVIDIA"; break;
        case 0x1002: gpuDriverVendor = "AMD"; break;
        case 0x13B5: gpuDriverVendor = "ARM"; break;
        case 0x5143: gpuDriverVendor = "Qualcomm"; break;
        case 0x1010: gpuDriverVendor = "Imagination Technologies"; break;
        default: gpuDriverVendor = "Unknown Vendor"; break;
    }
}

uint64_t GetProcessMemoryUsageBytes()
{
    return Platform::OS::GetProcessMemoryUsageBytes();
}

} // namespace Volcano
