#include "TickThread.hpp"
#include "CrashReporter.hpp"
#include "Logger.hpp"
#include "TickLoop.hpp"
#include "RideController.hpp"
#include "network/Connection.hpp"
#include "network/PacketIds.hpp"
#include "network/VarInt.hpp"
#include <chrono>
#include <cmath>

namespace Volcano {

namespace {

// Vanilla's own threshold for "did the player actually move this tick"
// (LocalPlayer::sendPosition: Mth.lengthSquared(dx, dy, dz) >
// Mth.square(2.0E-4)) - anything smaller isn't worth a packet.
constexpr double kMoveThresholdSquared = 2.0e-4 * 2.0e-4;

// ...and its forced-resend counter: 20 ticks (1 second) without a move still
// sends one, so a server that never hears from a perfectly still player
// doesn't lose track of them.
constexpr int kPositionReminderTicks = 20;

// MovementFlags: bit 0 onGround, bit 1 hasHorizontalCollision.
uint8_t MovementFlags(bool onGround, bool horizontalCollision)
{
    return static_cast<uint8_t>((onGround ? 0x01 : 0x00) | (horizontalCollision ? 0x02 : 0x00));
}

} // namespace

TickThread::~TickThread() = default; // jthread's own dtor requests stop + joins

void TickThread::Start()
{
    worker = std::jthread([this](std::stop_token st) { CrashReporter::InstallForThread(); ThreadEntry(st); });
}

void TickThread::ThreadEntry(std::stop_token stopToken)
{
    Log::Info("Tick thread created.");

    constexpr auto kTickInterval = std::chrono::milliseconds(50); // 1/20s, matching TickLoop::FIXED_DT.

    // Edge-triggered off worldReady exactly like the block this replaced in
    // NetworkClient::RunPlayLoop
    bool playerLoadedSent = false;

    // The "what did I last tell the server" state LocalPlayer::sendPosition
    // keeps (xLast/yLast/zLast, yRotLast/xRotLast, positionReminder,
    // lastOnGround, lastHorizontalCollision). Reset alongside
    // playerLoadedSent so a reconnect/dimension change starts clean rather
    // than diffing against the previous world's last report.
    glm::vec3 lastSentPosition{0.0f};
    float lastSentYaw = 0.0f;
    float lastSentPitch = 0.0f;
    int positionReminder = 0;
    bool lastOnGround = false;
    bool lastHorizontalCollision = false;

    // What the server was last told about sprinting and held movement keys
    // - only sent when these change, like vanilla's LocalPlayer.
    // sendIsSprintingIfNeeded/sendInput. The server's sprint flag is what
    // turns a full-strength hit into a knockback hit (see
    // InteractionManager::ClassifyAttack), so this has to stay current for
    // combat to work.
    bool reportedSprinting = false;
    uint8_t reportedInputFlags = 0;

    auto nextTick = std::chrono::steady_clock::now();

    while (!stopToken.stop_requested() && !state->shouldClose)
    {
        nextTick += kTickInterval;

        // No TickLoop::Tick() call here
        bool worldReady = state->worldReady.load();

        // Loaded via activeConnection rather than a member of this class:
        // the connection is owned by whichever NetworkClient is running this
        // session and can be replaced (reconnect) or vanish (disconnect) at
        // any time
        std::shared_ptr<Connection> connection = state->activeConnection.load();
        if (connection)
        {
            // worldReady gates the MOVEMENT packet only, never the tick
            // marker below it
            if (!worldReady)
            {
                playerLoadedSent = false;
                positionReminder = 0;
            }
            else
            {
                if (!playerLoadedSent)
                {
                    PacketWriter empty;
                    connection->QueuePacket(PlayC2S::PlayerLoaded, empty.Data());
                    playerLoadedSent = true;
                    Log::Info("Sent Player Loaded.");

                    // Seed the diff state from where we actually are, so the
                    // first real tick below compares against this spawn
                    // rather than against (0,0,0) and reports a bogus
                    // thousand-block "move" on its very first packet.
                    lastSentPosition = state->player->GetPosition();
                    lastSentYaw = std::remainder(-state->player->camera.GetYaw(), 360.0f);
                    lastSentPitch = -state->player->camera.GetPitch();
                    positionReminder = 0;
                }

                // The camera's yaw/pitch turn the opposite way from
                // Minecraft's own convention (see NetworkClient.cpp's
                // AngleByteToRadians
                glm::vec3 position = state->player->GetPosition();
                float yaw = std::remainder(-state->player->camera.GetYaw(), 360.0f);
                float pitch = -state->player->camera.GetPitch();
                bool onGround = state->tickLoop->IsGrounded();
                bool horizontalCollision = state->tickLoop->HasHorizontalCollision();

                // Straight port of LocalPlayer::sendPosition's decision: at
                // most ONE movement packet per tick, and the narrowest
                // variant that carries what actually changed. Sending the
                // widest one unconditionally every tick (what this used to
                // do) is not what a real client looks like on the wire.
                double deltaX = static_cast<double>(position.x) - static_cast<double>(lastSentPosition.x);
                double deltaY = static_cast<double>(position.y) - static_cast<double>(lastSentPosition.y);
                double deltaZ = static_cast<double>(position.z) - static_cast<double>(lastSentPosition.z);
                positionReminder++;

                bool moved = (deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ) > kMoveThresholdSquared
                    || positionReminder >= kPositionReminderTicks;
                bool rotated = yaw != lastSentYaw || pitch != lastSentPitch;

                uint8_t flags = MovementFlags(onGround, horizontalCollision);

                // Riding: RideController sends this tick's rotation-only move and
                // vehicle_move itself, so the position-bearing packets are skipped.
                bool riding = state->rideController != nullptr && state->rideController->QueueTickPackets(*connection);

                if (!riding && (moved || rotated))
                {
                    PacketWriter writer;
                    if (moved)
                    {
                        writer.WriteDouble(static_cast<double>(position.x));
                        writer.WriteDouble(static_cast<double>(position.y));
                        writer.WriteDouble(static_cast<double>(position.z));
                    }
                    if (rotated)
                    {
                        writer.WriteFloat(yaw);
                        writer.WriteFloat(pitch);
                    }
                    writer.WriteBytes(&flags, 1);

                    int32_t packetId = moved
                        ? (rotated ? PlayC2S::SetPlayerPositionAndRotation : PlayC2S::SetPlayerPosition)
                        : PlayC2S::SetPlayerRotation;
                    connection->QueuePacket(packetId, writer.Data());
                }
                else if (!riding && (onGround != lastOnGround || horizontalCollision != lastHorizontalCollision))
                {
                    // Vanilla's StatusOnly: nothing moved or turned, but the
                    // flags themselves changed and the server tracks those.
                    PacketWriter writer;
                    writer.WriteBytes(&flags, 1);
                    connection->QueuePacket(PlayC2S::SetPlayerMovementFlags, writer.Data());
                }
                // else: send no movement packet at all this tick, exactly as
                // a real standing-still client does.

                if (moved && !riding)
                {
                    lastSentPosition = position;
                    positionReminder = 0;
                }
                if (rotated)
                {
                    lastSentYaw = yaw;
                    lastSentPitch = pitch;
                }
                lastOnGround = onGround;
                lastHorizontalCollision = horizontalCollision;

                // Sprint/input state, queued (not written directly, like the
                // movement packet above) so it stays ordered after any
                // Attack packet the render thread already queued this tick -
                // a sprint hit has to reach the server before the
                // stop_sprinting it causes, or the server sees a plain hit.
                TickLoop::MovementSnapshot movement = state->tickLoop->GetMovementSnapshot();
                int32_t localEntityId = state->localEntityId.load();
                if (movement.sprinting != reportedSprinting && localEntityId >= 0)
                {
                    constexpr int32_t START_SPRINTING = 1, STOP_SPRINTING = 2;
                    PacketWriter writer;
                    writer.WriteVarInt(localEntityId);
                    writer.WriteVarInt(movement.sprinting ? START_SPRINTING : STOP_SPRINTING);
                    writer.WriteVarInt(0); // jumpBoost
                    connection->QueuePacket(PlayC2S::PlayerCommand, writer.Data());
                    reportedSprinting = movement.sprinting;
                }

                uint8_t inputFlags = state->tickLoop->GetMovementInputFlags();
                if (inputFlags != reportedInputFlags)
                {
                    PacketWriter writer;
                    writer.WriteBytes(&inputFlags, 1);
                    connection->QueuePacket(PlayC2S::PlayerInput, writer.Data());
                    reportedInputFlags = inputFlags;
                }
            }

            // Always last, and always sent
            PacketWriter tickEnd;
            connection->QueuePacket(PlayC2S::ClientTickEnd, tickEnd.Data());
        }

        std::this_thread::sleep_until(nextTick);
    }
}

} // namespace Volcano
