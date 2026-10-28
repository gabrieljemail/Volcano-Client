#pragma once
#ifndef VOLCANO_SCREENSHOT_H
#define VOLCANO_SCREENSHOT_H

#include <cstdint>
#include <string>
#include <vector>

namespace Volcano {

// F2 screenshots: writes an RGBA frame to <directory>/<date_time>.png on a
// background thread, so the encode never stalls rendering.
class Screenshot
{
public:
    // Returns the path the file will be written to.
    static std::string SaveAsync(std::vector<uint8_t> rgba, uint32_t width, uint32_t height, const std::string& directory);
};

} // namespace Volcano

#endif
