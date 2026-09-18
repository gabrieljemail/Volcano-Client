#include "SystemInfo.hpp"
#include <thread>

#ifdef _WIN32
#include <intrin.h>
#endif

namespace Volcano {

void SystemInfo::DetectCPU()
{
    cpuLogicalCores = std::thread::hardware_concurrency();

#ifdef _WIN32
    // CPUID leaves 0x80000002-0x80000004 return the processor brand string
    // as 48 raw ASCII bytes across three calls (EAX/EBX/ECX/EDX each call,
    // in that register order) — a real x86 CPUID feature both Intel and
    // AMD support, not an OS-specific registry/WMI query, and it already
    // includes the clock speed vanilla's own F3 screen shows (e.g.
    // "Intel(R) Core(TM) i5-6600 CPU @ 3.30GHz").
    int brand[12] = {};
    __cpuid(brand, static_cast<int>(0x80000000));
    if (static_cast<unsigned int>(brand[0]) >= 0x80000004u)
    {
        __cpuid(brand + 0, static_cast<int>(0x80000002));
        __cpuid(brand + 4, static_cast<int>(0x80000003));
        __cpuid(brand + 8, static_cast<int>(0x80000004));

        cpuModel.assign(reinterpret_cast<char*>(brand), sizeof(brand));

        // The buffer is null-terminated by CPUID itself but often has
        // trailing (and sometimes leading) spaces padded into it.
        size_t nul = cpuModel.find('\0');
        if (nul != std::string::npos) cpuModel.resize(nul);
        while (!cpuModel.empty() && cpuModel.back() == ' ') cpuModel.pop_back();
        size_t firstNonSpace = cpuModel.find_first_not_of(' ');
        cpuModel = (firstNonSpace == std::string::npos) ? "" : cpuModel.substr(firstNonSpace);
    }
#endif

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

} // namespace Volcano
