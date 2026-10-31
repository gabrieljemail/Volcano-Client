#pragma once
#ifndef VOLCANO_NETWORK_SKIN_FETCHER_H
#define VOLCANO_NETWORK_SKIN_FETCHER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace Volcano {

class Config;

// A downloaded skin as 64x64 RGBA8, legacy 64x32 skins already expanded.
struct SkinImage
{
    std::string key;
    std::vector<uint8_t> pixels;
    bool slim = false;
};

// Downloads skins on a worker thread. Request() is callable from any thread; the render thread collects the finished
// images with TakeCompleted() and uploads them. Only https addresses on Mojang's hosts (plus Skins.AllowedHosts in
// settings.json, for skin plugins that host their own) are fetched, since the URL comes from a server.
class SkinFetcher
{
public:
    explicit SkinFetcher(Config* config);
    ~SkinFetcher();

    // Queues a download unless `key` was requested before (failures included, so a bad URL isn't retried every frame).
    void Request(const std::string& key, const std::string& url, bool slim);

    std::vector<SkinImage> TakeCompleted();

    // Reads the url and model out of a "textures" profile property (base64 JSON). False when it has no skin.
    static bool ParseTexturesProperty(const std::string& base64Value, std::string& url, bool& slim);

private:
    struct Job
    {
        std::string key;
        std::string url;
        bool slim;
    };

    void Run(std::stop_token stopToken);
    bool HostAllowed(const std::string& url);
    bool Download(const Job& job, SkinImage& image, std::string& error);

    Config* config;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::deque<Job> queue;
    std::unordered_set<std::string> requested;
    std::vector<SkinImage> completed;
    std::jthread worker;
};

} // namespace Volcano

#endif
