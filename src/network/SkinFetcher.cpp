#include "SkinFetcher.hpp"
#include "Base64.hpp"
#include "../Config.hpp"
#include "../Logger.hpp"
#include "../platform/OS.hpp"
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <algorithm>

namespace Volcano {

namespace
{
using Json = nlohmann::json;

constexpr size_t MAX_SKIN_BYTES = 1024 * 1024;
constexpr int SKIN_SIZE = 64;

// Host part of an https URL, lowercase; empty for anything else.
std::string HostOf(const std::string& url)
{
    if (url.rfind("https://", 0) != 0) return {};
    size_t start = 8;
    size_t end = url.find_first_of("/:?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (url.find('@', start) != std::string::npos && url.find('@', start) < (end == std::string::npos ? url.size() : end)) return {};
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

bool HostMatches(const std::string& host, const std::string& allowed)
{
    if (allowed.empty()) return false;
    if (host == allowed) return true;
    return host.size() > allowed.size() && host.compare(host.size() - allowed.size(), allowed.size(), allowed) == 0
        && host[host.size() - allowed.size() - 1] == '.';
}

uint8_t* Pixel(std::vector<uint8_t>& pixels, int x, int y)
{
    return &pixels[(static_cast<size_t>(y) * SKIN_SIZE + static_cast<size_t>(x)) * 4];
}

// The base body layers are never see-through, whatever the file says.
void SetOpaque(std::vector<uint8_t>& pixels, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y < y1; y++)
    {
        for (int x = x0; x < x1; x++) Pixel(pixels, x, y)[3] = 255;
    }
}

// Vanilla's SkinTextureDownloader.copyRect: mirrors a rectangle onto the left limb's slot.
void CopyRect(std::vector<uint8_t>& pixels, int x, int y, int offsetX, int offsetY, int width, int height)
{
    for (int dy = 0; dy < height; dy++)
    {
        for (int dx = 0; dx < width; dx++)
        {
            const uint8_t* from = Pixel(pixels, x + dx, y + dy);
            uint8_t* to = Pixel(pixels, x + (width - 1 - dx) + offsetX, y + dy + offsetY);
            std::copy(from, from + 4, to);
        }
    }
}

// A legacy 64x32 skin has no left arm or leg; copy the right ones across. Skins without a transparent outer layer
// (an old convention) get it cleared so it doesn't draw as a solid shell.
void ExpandLegacy(std::vector<uint8_t>& pixels)
{
    bool hasTransparency = false;
    for (int y = 0; y < 32 && !hasTransparency; y++)
    {
        for (int x = 32; x < 64; x++)
        {
            if (Pixel(pixels, x, y)[3] < 128) hasTransparency = true;
        }
    }
    if (!hasTransparency)
    {
        for (int y = 0; y < 32; y++)
        {
            for (int x = 32; x < 64; x++) Pixel(pixels, x, y)[3] = 0;
        }
    }

    CopyRect(pixels, 4, 16, 16, 32, 4, 4);
    CopyRect(pixels, 8, 16, 16, 32, 4, 4);
    CopyRect(pixels, 0, 20, 24, 32, 4, 12);
    CopyRect(pixels, 4, 20, 16, 32, 4, 12);
    CopyRect(pixels, 8, 20, 8, 32, 4, 12);
    CopyRect(pixels, 12, 20, 16, 32, 4, 12);
    CopyRect(pixels, 44, 16, -8, 32, 4, 4);
    CopyRect(pixels, 48, 16, -8, 32, 4, 4);
    CopyRect(pixels, 40, 20, 0, 32, 4, 12);
    CopyRect(pixels, 44, 20, -8, 32, 4, 12);
    CopyRect(pixels, 48, 20, -16, 32, 4, 12);
    CopyRect(pixels, 52, 20, -8, 32, 4, 12);
}
} // namespace

SkinFetcher::SkinFetcher(Config* configIn) : config(configIn)
{
    worker = std::jthread([this](std::stop_token stopToken) { Run(stopToken); });
}

SkinFetcher::~SkinFetcher()
{
    worker.request_stop();
    wake.notify_all();
}

bool SkinFetcher::ParseTexturesProperty(const std::string& base64Value, std::string& url, bool& slim)
{
    std::vector<uint8_t> decoded = Base64Decode(base64Value);
    Json root = Json::parse(decoded.begin(), decoded.end(), nullptr, false);
    if (!root.is_object() || !root.contains("textures") || !root["textures"].is_object()) return false;

    const Json& textures = root["textures"];
    auto skin = textures.find("SKIN");
    if (skin == textures.end() || !skin->is_object() || !skin->contains("url") || !(*skin)["url"].is_string()) return false;

    url = (*skin)["url"].get<std::string>();
    slim = false;
    if (skin->contains("metadata") && (*skin)["metadata"].is_object())
    {
        slim = (*skin)["metadata"].value("model", std::string()) == "slim";
    }
    return !url.empty();
}

bool SkinFetcher::HostAllowed(const std::string& url)
{
    std::string host = HostOf(url);
    if (host.empty()) return false;
    for (const char* allowed : { "minecraft.net", "mojang.com", "minecraftservices.com" })
    {
        if (HostMatches(host, allowed)) return true;
    }
    if (config != nullptr)
    {
        for (const std::string& extra : config->GetStringArray("Skins.AllowedHosts"))
        {
            if (HostMatches(host, extra)) return true;
        }
    }
    return false;
}

void SkinFetcher::Request(const std::string& key, const std::string& url, bool slim)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!requested.insert(key).second) return;
        queue.push_back({ key, url, slim });
    }
    wake.notify_one();
}

std::vector<SkinImage> SkinFetcher::TakeCompleted()
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<SkinImage> out;
    out.swap(completed);
    return out;
}

bool SkinFetcher::Download(const Job& job, SkinImage& image, std::string& error)
{
    if (!HostAllowed(job.url))
    {
        error = "host isn't allowed (add it to Skins.AllowedHosts to trust it)";
        return false;
    }

    std::vector<uint8_t> body;
    if (!Platform::OS::HttpGet(job.url, body, error)) return false;
    if (body.size() > MAX_SKIN_BYTES)
    {
        error = "file is too large";
        return false;
    }

    int width = 0, height = 0, channels = 0;
    uint8_t* decoded = stbi_load_from_memory(body.data(), static_cast<int>(body.size()), &width, &height, &channels, 4);
    if (decoded == nullptr)
    {
        error = "not a readable image";
        return false;
    }
    if (width != SKIN_SIZE || (height != SKIN_SIZE && height != SKIN_SIZE / 2))
    {
        stbi_image_free(decoded);
        error = "unsupported skin size " + std::to_string(width) + "x" + std::to_string(height);
        return false;
    }

    image.key = job.key;
    image.slim = job.slim;
    image.pixels.assign(static_cast<size_t>(SKIN_SIZE) * SKIN_SIZE * 4, 0);
    std::copy(decoded, decoded + static_cast<size_t>(width) * height * 4, image.pixels.begin());
    stbi_image_free(decoded);

    if (height == SKIN_SIZE / 2) ExpandLegacy(image.pixels);
    SetOpaque(image.pixels, 0, 0, 32, 16);
    SetOpaque(image.pixels, 0, 16, 64, 32);
    SetOpaque(image.pixels, 16, 48, 48, 64);
    return true;
}

void SkinFetcher::Run(std::stop_token stopToken)
{
    while (!stopToken.stop_requested())
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, stopToken, [this] { return !queue.empty(); });
            if (stopToken.stop_requested()) return;
            job = std::move(queue.front());
            queue.pop_front();
        }

        SkinImage image;
        std::string error;
        if (Download(job, image, error))
        {
            std::lock_guard<std::mutex> lock(mutex);
            completed.push_back(std::move(image));
        }
        else
        {
            Log::Error("[SKIN] " + job.url + ": " + error);
        }
    }
}

} // namespace Volcano
