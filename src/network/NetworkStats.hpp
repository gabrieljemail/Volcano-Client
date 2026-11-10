#pragma once
#ifndef VOLCANO_NETWORK_STATS_H
#define VOLCANO_NETWORK_STATS_H

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace Volcano {

// Live counters for the F3 debug HUD's "Network" category
struct NetworkStats
{
    static constexpr size_t PACKET_ID_SLOTS = 256;

    std::atomic<uint64_t> packetsIn{0};
    std::atomic<uint64_t> packetsOut{0};
    std::atomic<uint64_t> bytesIn{0};   // Payload bytes, excluding framing/compression.
    std::atomic<uint64_t> bytesOut{0};
    // steady_clock ticks of the most recent packet in / out; 0 = none yet.
    std::atomic<int64_t> lastInTicks{0};
    std::atomic<int64_t> lastOutTicks{0};
    std::array<std::atomic<uint32_t>, PACKET_ID_SLOTS> inById{};
    std::array<std::atomic<uint32_t>, PACKET_ID_SLOTS> outById{};

    void RecordIn(int32_t packetId, size_t payloadBytes)
    {
        packetsIn.fetch_add(1, std::memory_order_relaxed);
        bytesIn.fetch_add(payloadBytes, std::memory_order_relaxed);
        lastInTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        if (packetId >= 0 && static_cast<size_t>(packetId) < PACKET_ID_SLOTS)
        {
            inById[static_cast<size_t>(packetId)].fetch_add(1, std::memory_order_relaxed);
        }
    }

    void RecordOut(int32_t packetId, size_t payloadBytes)
    {
        packetsOut.fetch_add(1, std::memory_order_relaxed);
        bytesOut.fetch_add(payloadBytes, std::memory_order_relaxed);
        lastOutTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        if (packetId >= 0 && static_cast<size_t>(packetId) < PACKET_ID_SLOTS)
        {
            outById[static_cast<size_t>(packetId)].fetch_add(1, std::memory_order_relaxed);
        }
    }

    // What the play loop is doing right now, for the stall watchdog and HUD.
    // Phase: 0 = idle/not in play, 1 = flushing outbound, 2 = waiting for data,
    // 3 = reading a packet, 4 = handling packet loopPacketId.
    std::atomic<int32_t> loopPhase{0};
    std::atomic<int32_t> loopPacketId{-1};
    std::atomic<int64_t> loopPhaseTicks{0};

    void SetLoopPhase(int32_t phase, int32_t packetId = -1)
    {
        loopPacketId.store(packetId, std::memory_order_relaxed);
        loopPhase.store(phase, std::memory_order_relaxed);
        loopPhaseTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }

    static const char* LoopPhaseName(int32_t phase)
    {
        switch (phase)
        {
        case 1: return "flushing outbound";
        case 2: return "waiting for data";
        case 3: return "reading a packet";
        case 4: return "handling a packet";
        default: return "idle";
        }
    }

    // Latest Play-state keep-alive: its id, how long after the previous one it
    // arrived, how long we took to answer, and when the answer went out.
    std::atomic<int64_t> keepAliveId{0};
    std::atomic<int64_t> keepAliveIdDelta{0};     // Id minus the previous id (the server's own clock, in ms).
    std::atomic<int64_t> keepAliveGapUs{-1};      // Wall time since the previous one arrived; -1 = first.
    std::atomic<int64_t> keepAliveReplyUs{-1};    // Receive-to-sent time of our answer; -1 = none yet.
    std::atomic<int64_t> keepAliveInTicks{0};     // steady_clock ticks when it arrived.
    std::atomic<int64_t> keepAliveOutTicks{0};    // steady_clock ticks when we answered.

    void RecordKeepAliveIn(int64_t id)
    {
        int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        int64_t previous = keepAliveInTicks.load(std::memory_order_relaxed);
        keepAliveGapUs.store(previous == 0 ? -1 : TicksToUs(now - previous), std::memory_order_relaxed);
        if (previous != 0) keepAliveIdDelta.store(id - keepAliveId.load(std::memory_order_relaxed), std::memory_order_relaxed);
        keepAliveId.store(id, std::memory_order_relaxed);
        keepAliveInTicks.store(now, std::memory_order_relaxed);
    }

    void RecordKeepAliveOut()
    {
        int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        keepAliveReplyUs.store(TicksToUs(now - keepAliveInTicks.load(std::memory_order_relaxed)), std::memory_order_relaxed);
        keepAliveOutTicks.store(now, std::memory_order_relaxed);
    }

    static int64_t TicksToUs(int64_t ticks)
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::duration(ticks)).count();
    }

    // Milliseconds since a Last*Ticks value, or -1 if nothing has been recorded.
    static double AgeMs(int64_t ticks)
    {
        if (ticks == 0) return -1.0;
        auto then = std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ticks));
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - then).count();
    }
};

// One for the process: the HUD reads whichever connection is current, and a
// reconnect just keeps counting, so deltas stay meaningful across sessions.
inline NetworkStats networkStats;

} // namespace Volcano

#endif
