#include "RideController.hpp"
#include "FluidQuery.hpp"
#include "inventory/ItemRegistry.hpp"
#include "network/Connection.hpp"
#include "network/PacketIds.hpp"
#include "network/VarInt.hpp"
#include "renderer/terrain/models/BlockRegistry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace Volcano {

namespace {

constexpr float DEG_TO_RAD = 0.017453292f;
constexpr float GRAVITY = 0.08f;
constexpr float STEP_SECONDS = 1.0f / 20.0f;

// Vanilla's boat floats (AbstractBoat.floatBoat) and steers (controlBoat).
constexpr float BOAT_GRAVITY = -0.04f;
constexpr float BOAT_BUOYANCY = 0.06153846f;
constexpr float BOAT_FORWARD = 0.04f;
constexpr float BOAT_TURN_ONLY = 0.005f;
constexpr float BOAT_BACKWARD = 0.005f;

// Ridden horse and pig friction, from the default 0.6 block slipperiness.
constexpr float GROUND_FRICTION = 0.546f;
constexpr float AIR_FRICTION = 0.91f;
constexpr float INPUT_SCALE = 0.98f; // Vanilla scales player input by this before it reaches a mount.
constexpr float WATER_DRAG = 0.8f;
constexpr float WATER_ACCELERATION = 0.02f;
constexpr float PIG_RIDDEN_SPEED = 0.225f;

constexpr float DEFAULT_HORSE_SPEED = 0.225f;
constexpr float DEFAULT_PIG_SPEED = 0.25f;
constexpr float DEFAULT_JUMP_STRENGTH = 0.7f;

// A float nudged to never land past the contact surface, as TickLoop does.
float RoundTowardOrigin(double target, bool movingPositive)
{
    float f = static_cast<float>(target);
    if (movingPositive && static_cast<double>(f) > target)
        f = std::nextafter(f, std::numeric_limits<float>::lowest());
    else if (!movingPositive && static_cast<double>(f) < target)
        f = std::nextafter(f, std::numeric_limits<float>::max());
    return f;
}

float WrapDegrees(float degrees)
{
    return std::remainder(degrees, 360.0f);
}

} // namespace

void RideController::ApplyServerMove(glm::vec3 position, float yawDegrees, float pitchDegrees)
{
    (void)pitchDegrees;
    std::lock_guard<std::mutex> lock(correctionMutex);
    correction = Correction{ position, yawDegrees };
}

bool RideController::HoldsSteeringItem() const
{
    auto isSteering = [](const ItemStack& stack)
    {
        if (stack.IsEmpty()) return false;
        const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(stack.itemId);
        return item != nullptr && (item->name == "carrot_on_a_stick" || item->name == "warped_fungus_on_a_stick");
    };

    std::lock_guard<std::mutex> lock(state->inventory.mutex);
    return isSteering(state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot))
        || isSteering(state->inventory.offhand.Get(0));
}

void RideController::Begin(const Entity& vehicle)
{
    activeId = static_cast<int32_t>(vehicle.id);
    kind = EntitySeat::KindOf(vehicle);
    position = vehicle.position;
    velocity = glm::vec3(0.0f);
    yawDeg = -vehicle.yaw / DEG_TO_RAD;
    pitchDeg = 0.0f;
    deltaRotation = 0.0f;
    grounded = false;
    horizontalCollision = false;
    wasJumpHeld = false;
    jumpHoldTicks = 0;
    pendingJump = 0.0f;
    queuedJumpBoost = -1;
    paddleLeft = paddleRight = false;
    std::lock_guard<std::mutex> lock(correctionMutex);
    correction.reset();
}

void RideController::Release()
{
    if (activeId >= 0)
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        if (Entity* vehicle = state->FindEntity(static_cast<uint32_t>(activeId))) vehicle->locallyControlled = false;
    }
    activeId = -1;
    std::lock_guard<std::mutex> lock(packetMutex);
    packet = Packet{};
}

RideController::Result RideController::Step(const Input& input)
{
    Result result;
    int32_t id = state->ridingEntityId.load();
    if (id < 0)
    {
        if (activeId >= 0) Release();
        return result;
    }
    if (id != activeId && activeId >= 0) Release();

    result.riding = true;
    result.playerPosition = state->player->GetPosition();

    bool steeringItem = HoldsSteeringItem();
    glm::vec3 followPosition{0.0f};
    float followYaw = 0.0f;
    int seatIndex = 0;
    int seatCount = 1;
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        Entity* vehicle = state->FindEntity(static_cast<uint32_t>(id));
        if (vehicle == nullptr) return result;

        if (id != activeId) Begin(*vehicle);
        halfWidth = vehicle->boundingBox.x * 0.5f;
        height = vehicle->boundingBox.y;
        movementSpeed = vehicle->movementSpeed;
        jumpStrength = vehicle->jumpStrength;
        seatCount = std::max(static_cast<int>(vehicle->passengers.size()), 1);
        seatIndex = EntitySeat::LocalSeatIndex(state, *vehicle);

        controlled = kind == EntitySeat::Kind::Boat || kind == EntitySeat::Kind::Horse
            || (kind == EntitySeat::Kind::Pig && steeringItem);
        vehicle->locallyControlled = controlled;
        followPosition = vehicle->InterpolatedPosition(std::chrono::steady_clock::now());
        followYaw = -vehicle->InterpolatedYaw(std::chrono::steady_clock::now()) / DEG_TO_RAD;
    }

    const glm::vec3 previousPosition = position;
    const float previousYawDeg = yawDeg;

    if (!controlled)
    {
        position = followPosition;
        yawDeg = followYaw;
        velocity = glm::vec3(0.0f);
        std::lock_guard<std::mutex> lock(packetMutex);
        packet = Packet{};
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(correctionMutex);
            if (correction)
            {
                position = correction->position;
                yawDeg = correction->yawDeg;
                correction.reset();
            }
        }

        stepHeight = kind == EntitySeat::Kind::Horse ? 1.0f : (kind == EntitySeat::Kind::Boat ? 0.0f : 0.6f);
        if (kind == EntitySeat::Kind::Boat) StepBoat(input);
        else StepMount(input);

        std::lock_guard<std::mutex> lock(packetMutex);
        packet.steered = true;
        packet.boat = kind == EntitySeat::Kind::Boat;
        packet.position = position;
        packet.yawDeg = WrapDegrees(yawDeg);
        packet.pitchDeg = pitchDeg;
        packet.onGround = grounded;
        packet.horizontalCollision = horizontalCollision;
        packet.paddleLeft = paddleLeft;
        packet.paddleRight = paddleRight;
        if (queuedJumpBoost >= 0) packet.jumpBoost = queuedJumpBoost;
        queuedJumpBoost = -1;
    }

    // Publish the pose so the renderer draws the vehicle where we put it.
    if (controlled)
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        if (Entity* vehicle = state->FindEntity(static_cast<uint32_t>(id)))
        {
            vehicle->previousPosition = previousPosition;
            vehicle->previousYaw = -previousYawDeg * DEG_TO_RAD;
            vehicle->position = position;
            vehicle->yaw = -yawDeg * DEG_TO_RAD;
            vehicle->velocity = velocity;
            vehicle->correction = glm::vec3(0.0f);
            vehicle->lastUpdateTime = std::chrono::steady_clock::now();
            vehicle->interpSeconds = STEP_SECONDS;
        }
    }

    float heading = -yawDeg * DEG_TO_RAD;
    result.playerPosition = EntitySeat::SeatPosition(kind, position, heading, height, seatIndex, seatCount);
    result.yawDelta = kind == EntitySeat::Kind::Boat ? -WrapDegrees(yawDeg - previousYawDeg) : 0.0f;
    return result;
}

bool RideController::QueueTickPackets(Connection& connection)
{
    if (state->ridingEntityId.load() < 0) return false;

    Packet snapshot;
    {
        std::lock_guard<std::mutex> lock(packetMutex);
        snapshot = packet;
        packet.jumpBoost = -1;
    }

    // A passenger only reports where it looks; the vehicle carries the position.
    {
        PacketWriter writer;
        writer.WriteFloat(WrapDegrees(-state->player->camera.GetYaw()));
        writer.WriteFloat(-state->player->camera.GetPitch());
        uint8_t flags = static_cast<uint8_t>((snapshot.onGround ? 0x01 : 0x00) | (snapshot.horizontalCollision ? 0x02 : 0x00));
        writer.WriteBytes(&flags, 1);
        connection.QueuePacket(PlayC2S::SetPlayerRotation, writer.Data());
    }

    if (snapshot.steered)
    {
        PacketWriter writer;
        writer.WriteDouble(static_cast<double>(snapshot.position.x));
        writer.WriteDouble(static_cast<double>(snapshot.position.y));
        writer.WriteDouble(static_cast<double>(snapshot.position.z));
        writer.WriteFloat(snapshot.yawDeg);
        writer.WriteFloat(snapshot.pitchDeg);
        writer.WriteBool(snapshot.onGround);
        connection.QueuePacket(PlayC2S::VehicleMove, writer.Data());

        if (snapshot.boat)
        {
            PacketWriter paddles;
            paddles.WriteBool(snapshot.paddleLeft);
            paddles.WriteBool(snapshot.paddleRight);
            connection.QueuePacket(PlayC2S::SteerBoat, paddles.Data());
        }
    }

    int32_t localEntityId = state->localEntityId.load();
    if (snapshot.jumpBoost >= 0 && localEntityId >= 0)
    {
        constexpr int32_t START_HORSE_JUMP = 3;
        PacketWriter writer;
        writer.WriteVarInt(localEntityId);
        writer.WriteVarInt(START_HORSE_JUMP);
        writer.WriteVarInt(snapshot.jumpBoost);
        connection.QueuePacket(PlayC2S::PlayerCommand, writer.Data());
    }
    return true;
}

void RideController::StepBoat(const Input& input)
{
    FluidQuery fluids(state->world);

    // Water around the hull: surface height, and whether the top is submerged.
    bool inWater = false;
    float waterLevel = -std::numeric_limits<float>::max();
    constexpr float SHRINK = 0.001f;
    int minX = static_cast<int>(std::floor(position.x - halfWidth + SHRINK));
    int maxX = static_cast<int>(std::floor(position.x + halfWidth - SHRINK));
    int minZ = static_cast<int>(std::floor(position.z - halfWidth + SHRINK));
    int maxZ = static_cast<int>(std::floor(position.z + halfWidth - SHRINK));
    int minY = static_cast<int>(std::floor(position.y + SHRINK));
    int maxY = static_cast<int>(std::floor(position.y + height - SHRINK));
    for (int y = minY; y <= maxY; y++)
    {
        for (int z = minZ; z <= maxZ; z++)
        {
            for (int x = minX; x <= maxX; x++)
            {
                FluidQuery::Cell cell = fluids.At(x, y, z);
                if (cell.kind != FluidQuery::WATER) continue;
                float surface = static_cast<float>(y) + cell.height;
                waterLevel = std::max(waterLevel, surface);
                inWater = inWater || position.y < surface;
            }
        }
    }
    bool underWater = fluids.Contains(position + glm::vec3(0.0f, height + SHRINK, 0.0f));
    bool onLand = !inWater && !underWater && Sweep(position, 1, -0.001) > -0.001;

    // floatBoat: gravity, buoyancy, and the drag that also damps the turn.
    float friction = 0.9f;
    float lift = 0.0f;
    if (underWater)
    {
        friction = 0.45f;
        lift = 0.01f;
    }
    else if (inWater)
    {
        lift = (waterLevel - position.y) / height;
    }
    else if (onLand)
    {
        friction = 0.6f;
    }
    velocity.x *= friction;
    velocity.z *= friction;
    velocity.y += BOAT_GRAVITY;
    deltaRotation *= friction;
    if (lift > 0.0f) velocity.y = (velocity.y + lift * BOAT_BUOYANCY) * 0.75f;

    // controlBoat: rowing turns the hull, forward and back push it.
    bool up = input.inputAllowed && input.forward > 0.5f;
    bool down = input.inputAllowed && input.forward < -0.5f;
    bool left = input.inputAllowed && input.right < -0.5f;
    bool right = input.inputAllowed && input.right > 0.5f;
    if (left) deltaRotation -= 1.0f;
    if (right) deltaRotation += 1.0f;
    float push = 0.0f;
    if (right != left && !up && !down) push += BOAT_TURN_ONLY;
    yawDeg += deltaRotation;
    if (up) push += BOAT_FORWARD;
    if (down) push -= BOAT_BACKWARD;
    float heading = -yawDeg * DEG_TO_RAD;
    velocity.x += std::sin(heading) * push;
    velocity.z += std::cos(heading) * push;
    paddleLeft = (right && !left) || up;
    paddleRight = (left && !right) || up;
    pitchDeg = 0.0f;

    Move(position, velocity);
}

void RideController::UpdateJumpCharge(const Input& input)
{
    bool jumpHeld = input.inputAllowed && input.jumpHeld;
    if (jumpHeld)
    {
        if (!wasJumpHeld) jumpHoldTicks = 0;
        jumpHoldTicks++;
    }
    else if (wasJumpHeld && jumpHoldTicks > 0)
    {
        // Release: vanilla's charge curve, sent as 0-100 and applied locally.
        float scale = jumpHoldTicks < 10 ? jumpHoldTicks * 0.1f : 0.8f + 2.0f / static_cast<float>(jumpHoldTicks - 9) * 0.1f;
        int boost = static_cast<int>(std::floor(scale * 100.0f));
        queuedJumpBoost = boost;
        pendingJump = boost >= 90 ? 1.0f : 0.4f + 0.4f * static_cast<float>(boost) / 90.0f;
        jumpHoldTicks = 0;
    }
    wasJumpHeld = jumpHeld;
}

void RideController::StepMount(const Input& input)
{
    FluidQuery fluids(state->world);

    // The mount faces where the rider looks.
    yawDeg = -state->player->camera.GetYaw();
    pitchDeg = -state->player->camera.GetPitch() * 0.5f;
    float yawRad = yawDeg * DEG_TO_RAD;

    bool horse = kind == EntitySeat::Kind::Horse;
    float speed = movementSpeed > 0.0f ? movementSpeed : (horse ? DEFAULT_HORSE_SPEED : DEFAULT_PIG_SPEED);
    if (!horse) speed *= PIG_RIDDEN_SPEED;

    // Horses strafe at half rate and back up slowly; a carrot-driven pig just goes forward.
    float forward = horse ? input.forward : 1.0f;
    float strafe = horse ? -input.right * 0.5f : 0.0f;
    if (!input.inputAllowed)
    {
        forward = horse ? 0.0f : 1.0f;
        strafe = 0.0f;
    }
    if (horse && forward <= 0.0f) forward *= 0.25f;
    forward *= INPUT_SCALE;
    strafe *= INPUT_SCALE;

    if (horse) UpdateJumpCharge(input);

    float submerged = fluids.SubmergedFraction(position, height);
    bool inWater = submerged > 0.0f;

    // Ridden jump: the charge picked when the key was released, once on the ground.
    if (horse && pendingJump > 0.0f && grounded && !inWater)
    {
        float strength = jumpStrength > 0.0f ? jumpStrength : DEFAULT_JUMP_STRENGTH;
        velocity.y = strength * pendingJump;
        if (forward > 0.0f)
        {
            velocity.x += -0.4f * std::sin(yawRad) * pendingJump;
            velocity.z += 0.4f * std::cos(yawRad) * pendingJump;
        }
        grounded = false;
        pendingJump = 0.0f;
    }
    else if (grounded)
    {
        pendingJump = 0.0f;
    }

    // moveRelative: input vector scaled by acceleration, turned to the mount's yaw.
    glm::vec2 wish(strafe, forward);
    float length = glm::length(wish);
    if (length > 1.0f) wish /= length;
    float acceleration = inWater ? WATER_ACCELERATION : (grounded ? speed : speed * 0.1f);
    wish *= acceleration;
    float sine = std::sin(yawRad);
    float cosine = std::cos(yawRad);
    velocity.x += wish.x * cosine - wish.y * sine;
    velocity.z += wish.y * cosine + wish.x * sine;

    Move(position, velocity);

    if (inWater)
    {
        velocity.x *= WATER_DRAG;
        velocity.z *= WATER_DRAG;
        velocity.y = velocity.y * WATER_DRAG - GRAVITY / 16.0f;
        if (submerged > 0.4f) velocity.y += 0.04f; // Mobs keep their heads above water.
    }
    else
    {
        float friction = grounded ? GROUND_FRICTION : AIR_FRICTION;
        velocity.x *= friction;
        velocity.z *= friction;
        velocity.y = (velocity.y - GRAVITY) * 0.98f;
    }
}

// Same sweep as TickLoop::SweepAxis, against this vehicle's box.
double RideController::Sweep(glm::vec3 at, int axis, double delta) const
{
    constexpr double TOUCH = 1e-7;
    if (delta == 0.0) return 0.0;

    const double extentMin[3] = {
        static_cast<double>(at.x) - halfWidth, static_cast<double>(at.y), static_cast<double>(at.z) - halfWidth };
    const double extentMax[3] = {
        static_cast<double>(at.x) + halfWidth, static_cast<double>(at.y) + height, static_cast<double>(at.z) + halfWidth };

    int lo[3], hi[3];
    for (int i = 0; i < 3; i++)
    {
        double a = extentMin[i], b = extentMax[i];
        if (i == axis)
        {
            a = std::min(a, a + delta);
            b = std::max(b, b + delta);
        }
        lo[i] = static_cast<int>(std::floor(a));
        hi[i] = static_cast<int>(std::floor(b));
    }
    lo[1] -= 1; // Fences and walls reach into the cell above their own.

    const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;

    for (int by = lo[1]; by <= hi[1]; by++)
    {
        for (int bz = lo[2]; bz <= hi[2]; bz++)
        {
            for (int bx = lo[0]; bx <= hi[0]; bx++)
            {
                BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(state->world->GetBlock(bx, by, bz));
                if (shape.count == 0) continue;

                const double origin[3] = { static_cast<double>(bx), static_cast<double>(by), static_cast<double>(bz) };
                for (int i = 0; i < shape.count; i++)
                {
                    const BlockRegistry::AABB& box = shape.boxes[static_cast<size_t>(i)];
                    double boxMin[3], boxMax[3];
                    for (int k = 0; k < 3; k++)
                    {
                        boxMin[k] = origin[k] + box.min[k];
                        boxMax[k] = origin[k] + box.max[k];
                    }

                    if (!(extentMin[o1] < boxMax[o1] && extentMax[o1] > boxMin[o1])) continue;
                    if (!(extentMin[o2] < boxMax[o2] && extentMax[o2] > boxMin[o2])) continue;

                    if (delta > 0.0 && extentMax[axis] <= boxMin[axis] + TOUCH)
                        delta = std::min(delta, std::max(boxMin[axis] - extentMax[axis], 0.0));
                    else if (delta < 0.0 && extentMin[axis] >= boxMax[axis] - TOUCH)
                        delta = std::max(delta, std::min(boxMax[axis] - extentMin[axis], 0.0));
                }
            }
        }
    }
    return delta;
}

void RideController::MoveAxis(glm::vec3& at, glm::vec3& vel, int axis, float delta)
{
    if (delta == 0.0f) return;

    const double requested = delta;
    const double allowed = Sweep(at, axis, requested);
    const bool blocked = allowed != requested;

    if (axis == 1)
    {
        at.y = RoundTowardOrigin(static_cast<double>(at.y) + allowed, delta > 0.0f);
        if (blocked)
        {
            if (delta < 0.0f) grounded = true;
            vel.y = 0.0f;
        }
        else
        {
            grounded = false;
        }
        return;
    }

    glm::vec3 result = at;
    result[axis] = RoundTowardOrigin(static_cast<double>(at[axis]) + allowed, delta > 0.0f);
    double moved = allowed;

    if (blocked && grounded && stepHeight > 0.0f)
    {
        // Step up, move, drop back down; kept only if it got further.
        const double up = Sweep(at, 1, stepHeight);
        glm::vec3 stepped = at;
        stepped.y = RoundTowardOrigin(static_cast<double>(at.y) + up, true);

        const double steppedAllowed = Sweep(stepped, axis, requested);
        if (std::fabs(steppedAllowed) > std::fabs(allowed))
        {
            stepped[axis] = RoundTowardOrigin(static_cast<double>(at[axis]) + steppedAllowed, delta > 0.0f);
            const double down = Sweep(stepped, 1, -static_cast<double>(stepped.y - at.y));
            stepped.y = RoundTowardOrigin(static_cast<double>(stepped.y) + down, false);
            result = stepped;
            moved = steppedAllowed;
        }
    }

    at = result;
    if (moved != requested)
    {
        vel[axis] = 0.0f;
        horizontalCollision = true;
    }
}

void RideController::Move(glm::vec3& at, glm::vec3& vel)
{
    horizontalCollision = false;
    MoveAxis(at, vel, 1, vel.y);
    MoveAxis(at, vel, 0, vel.x);
    MoveAxis(at, vel, 2, vel.z);
}

} // namespace Volcano
