#include "models/PacketEvent.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <utility>
#include "../Logger.hpp"

namespace Volcano {

namespace {

struct Entry {
    uint32_t id;
    PacketListener listener;
};

std::mutex listenersMutex; // Guards listeners and nextId; held only to copy or edit the list.
std::vector<Entry> listeners;
uint32_t nextId = 1;
std::atomic<uint32_t> listenerCount{ 0 };
std::atomic<bool> playActive{ false };

} // namespace

uint32_t PacketEvents::AddListener(PacketListener listener)
{
    if (!listener) return 0;
    std::lock_guard<std::mutex> lock(listenersMutex);
    uint32_t id = nextId++;
    listeners.push_back(Entry{ id, std::move(listener) });
    listenerCount.store(static_cast<uint32_t>(listeners.size()), std::memory_order_relaxed);
    return id;
}

bool PacketEvents::RemoveListener(uint32_t id)
{
    std::lock_guard<std::mutex> lock(listenersMutex);
    auto it = std::find_if(listeners.begin(), listeners.end(), [id](const Entry& e) { return e.id == id; });
    if (it == listeners.end()) return false;
    listeners.erase(it);
    listenerCount.store(static_cast<uint32_t>(listeners.size()), std::memory_order_relaxed);
    return true;
}

bool PacketEvents::HasListeners()
{
    return listenerCount.load(std::memory_order_relaxed) > 0;
}

bool PacketEvents::Dispatch(PacketEvent& event)
{
    if (!HasListeners() || !IsPlayActive()) return true;

    // Copy so a listener may remove itself while running.
    std::vector<Entry> snapshot;
    {
        std::lock_guard<std::mutex> lock(listenersMutex);
        snapshot = listeners;
    }

    for (const Entry& entry : snapshot)
    {
        try
        {
            entry.listener(event);
        }
        catch (const std::exception& e)
        {
            Log::Error(std::string("[NET] Packet listener threw: ") + e.what());
        }
        catch (...)
        {
            Log::Error("[NET] Packet listener threw.");
        }
        if (event.cancelled) return false;
    }
    return true;
}

void PacketEvents::SetPlayActive(bool active)
{
    playActive.store(active, std::memory_order_relaxed);
}

bool PacketEvents::IsPlayActive()
{
    return playActive.load(std::memory_order_relaxed);
}

} // namespace Volcano
