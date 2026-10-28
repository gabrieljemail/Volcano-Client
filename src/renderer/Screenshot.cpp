#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "Screenshot.hpp"
#include "../CrashReporter.hpp"
#include "Logger.hpp"
#include <chrono>
#include <ctime>
#include <filesystem>
#include <thread>

namespace Volcano {

std::string Screenshot::SaveAsync(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, const std::string& directory)
{
    // Vanilla's naming: 2026-09-30_14.05.33.png, with _N appended on collision.
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H.%M.%S", &local);

    std::filesystem::create_directories(directory);
    std::string path = directory + "/" + stamp + ".png";
    for (int suffix = 1; std::filesystem::exists(path); suffix++)
    {
        path = directory + "/" + stamp + "_" + std::to_string(suffix) + ".png";
    }

    std::thread([rgba = std::move(rgba), width, height, path]
    {
        CrashReporter::InstallForThread();
        int stride = static_cast<int>(width) * 4;
        if (stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), stride))
        {
            Log::Info("Saved screenshot: " + path);
        }
        else
        {
            Log::Error("Failed to save screenshot: " + path);
        }
    }).detach();

    return path;
}

} // namespace Volcano
