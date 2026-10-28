#include "ParticleSystem.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <variant>
#include <nlohmann/json.hpp>
#include "../../Config.hpp"
#include "../../GlobalState.hpp"
#include "../../Logger.hpp"
#include "../terrain/models/World.hpp"

namespace Volcano {

namespace {

constexpr const char* PARTICLES_JSON = "resources/minecraft-data/data/pc/26.1/particles.json";
constexpr float GRAVITY_PER_TICK = 0.04f;   // What a gravity of 1 pulls by, in blocks per tick squared.
constexpr size_t MAX_QUEUED = 4096;
constexpr int32_t MAX_PER_REQUEST = 512;

using Velocity = ParticleProfile::Velocity;
using Shrink = ParticleProfile::Shrink;

// How each type behaves, roughly as vanilla's classes do. Anything not here is a plain drifting sprite.
ParticleProfile ProfileFor(const std::string& name)
{
    if (name == "flame" || name == "soul_fire_flame" || name == "copper_fire_flame")
        return { .minLife = 8, .maxLife = 24, .size = 0.1f, .friction = 0.96f, .velocity = Velocity::Direct, .shrink = Shrink::Flame, .bright = true };
    if (name == "small_flame")
        return { .minLife = 8, .maxLife = 24, .size = 0.05f, .friction = 0.96f, .velocity = Velocity::Direct, .shrink = Shrink::Flame, .bright = true };
    if (name == "smoke")
        return { .minLife = 12, .maxLife = 30, .size = 0.1f, .gravity = -0.1f, .friction = 0.96f, .fades = true, .tint = { 0.2f, 0.2f, 0.2f } };
    if (name == "large_smoke")
        return { .minLife = 16, .maxLife = 40, .size = 0.15f, .gravity = -0.1f, .friction = 0.96f, .fades = true, .tint = { 0.2f, 0.2f, 0.2f } };
    if (name == "white_smoke")
        return { .minLife = 12, .maxLife = 30, .size = 0.1f, .gravity = -0.1f, .friction = 0.96f, .fades = true, .tint = { 0.85f, 0.85f, 0.85f } };
    if (name == "campfire_cosy_smoke" || name == "campfire_signal_smoke")
        return { .minLife = 100, .maxLife = 200, .size = 0.3f, .gravity = -0.02f, .friction = 0.99f, .velocity = Velocity::Direct, .fades = true, .collides = false };
    if (name == "poof")
        return { .minLife = 6, .maxLife = 12, .size = 0.2f, .friction = 0.9f, .velocity = Velocity::Direct, .fades = true, .bright = true, .tint = { 0.9f, 0.9f, 0.9f } };
    if (name == "cloud")
        return { .minLife = 8, .maxLife = 16, .size = 0.15f, .friction = 0.96f, .velocity = Velocity::Direct, .fades = true, .bright = true };
    if (name == "crit")
        return { .minLife = 3, .maxLife = 7, .size = 0.1f, .gravity = 0.2f, .friction = 0.7f, .shrink = Shrink::Linear };
    if (name == "enchanted_hit")
        return { .minLife = 3, .maxLife = 7, .size = 0.1f, .gravity = 0.2f, .friction = 0.7f, .shrink = Shrink::Linear, .tint = { 0.3f, 0.8f, 1.0f } };
    if (name == "damage_indicator")
        return { .minLife = 16, .maxLife = 24, .size = 0.15f, .gravity = -0.15f, .friction = 0.9f, .velocity = Velocity::Direct, .fades = true, .bright = true };
    if (name == "heart")
        return { .minLife = 10, .maxLife = 22, .size = 0.15f, .gravity = -0.03f, .friction = 0.86f, .velocity = Velocity::Direct, .bright = true, .collides = false };
    if (name == "angry_villager")
        return { .minLife = 12, .maxLife = 20, .size = 0.15f, .friction = 0.86f, .velocity = Velocity::Direct, .bright = true, .collides = false };
    if (name == "happy_villager" || name == "composter")
        return { .minLife = 20, .maxLife = 40, .size = 0.1f, .friction = 0.99f, .velocity = Velocity::Direct };
    if (name == "note")
        return { .minLife = 6, .maxLife = 14, .size = 0.15f, .friction = 0.66f, .velocity = Velocity::Direct, .bright = true, .collides = false };
    if (name == "portal" || name == "reverse_portal")
        return { .minLife = 40, .maxLife = 50, .size = 0.1f, .friction = 1.0f, .velocity = Velocity::Direct, .bright = true, .collides = false, .tint = { 0.9f, 0.3f, 1.0f } };
    if (name == "bubble")
        return { .minLife = 10, .maxLife = 30, .size = 0.05f, .gravity = -0.05f, .friction = 0.85f, .velocity = Velocity::Direct, .collides = false };
    if (name == "splash")
        return { .minLife = 8, .maxLife = 14, .size = 0.1f, .gravity = 1.0f, .friction = 0.95f };
    if (name == "rain")
        return { .minLife = 6, .maxLife = 12, .size = 0.05f, .gravity = 1.0f, .friction = 0.95f, .velocity = Velocity::Direct, .fades = true };
    if (name == "end_rod")
        return { .minLife = 60, .maxLife = 72, .size = 0.075f, .jitterSize = false, .friction = 0.91f, .velocity = Velocity::Direct, .fades = true, .bright = true, .collides = false };
    if (name == "dust")
        return { .minLife = 8, .maxLife = 40, .size = 0.1f, .friction = 0.96f, .tintFromPacket = true };
    if (name == "entity_effect" || name == "effect" || name == "instant_effect" || name == "ambient_entity_effect")
        return { .minLife = 8, .maxLife = 40, .size = 0.1f, .gravity = -0.1f, .friction = 0.96f, .tintFromPacket = true };
    if (name == "totem_of_undying")
        return { .minLife = 60, .maxLife = 70, .size = 0.15f, .gravity = 0.4f, .friction = 0.6f, .tint = { 0.75f, 0.9f, 0.3f } };
    if (name == "explosion")
        return { .minLife = 6, .maxLife = 9, .size = 1.0f, .jitterSize = false, .friction = 1.0f, .velocity = Velocity::Direct, .bright = true, .collides = false };
    if (name == "sweep_attack")
        return { .minLife = 4, .maxLife = 4, .size = 1.0f, .jitterSize = false, .friction = 1.0f, .velocity = Velocity::Direct, .bright = true, .collides = false };
    return {};
}

} // namespace

bool ParticleSystem::Init()
{
    bool loaded = sprites.Load();

    std::ifstream file(PARTICLES_JSON);
    nlohmann::json list = nlohmann::json::parse(file, nullptr, false);
    if (!list.is_array())
    {
        Log::Error(std::string("Particles: couldn't read ") + PARTICLES_JSON);
        return false;
    }
    for (const nlohmann::json& entry : list)
    {
        if (!entry.is_object() || !entry.contains("id") || !entry.contains("name")) continue;
        int32_t id = entry["id"].get<int32_t>();
        if (id < 0 || id > 4096) continue;
        if (static_cast<size_t>(id) >= types.size()) types.resize(static_cast<size_t>(id) + 1);

        Type& type = types[static_cast<size_t>(id)];
        type.name = entry["name"].get<std::string>();
        type.profile = ProfileFor(type.name);
        type.spriteList = sprites.Of(type.name);
        typeIds[type.name] = id;
    }
    return loaded && !types.empty();
}

int32_t ParticleSystem::TypeId(const std::string& name) const
{
    auto found = typeIds.find(name);
    return found != typeIds.end() ? found->second : -1;
}

const std::string& ParticleSystem::TypeName(int32_t id) const
{
    static const std::string none;
    return id >= 0 && static_cast<size_t>(id) < types.size() ? types[static_cast<size_t>(id)].name : none;
}

void ParticleSystem::Spawn(const ParticleSpawn& request)
{
    std::lock_guard<std::mutex> lock(pendingMutex);
    if (pending.size() < MAX_QUEUED) pending.push_back(request);
}

void ParticleSystem::Update(GlobalState* state, float deltaSeconds)
{
    // The particle setting: All, Decreased or Minimal keeps this share of what is asked for.
    float keep = 1.0f;
    if (state != nullptr && state->config != nullptr)
    {
        uint32_t level = std::get<uint32_t>(state->config->Get("Graphics.Particles", uint32_t{0}));
        keep = level == 0 ? 1.0f : level == 1 ? 0.33f : 0.1f;
    }

    std::vector<ParticleSpawn> requests;
    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        requests.swap(pending);
    }
    for (const ParticleSpawn& request : requests) Emit(request, keep);

    // A stall doesn't make it catch up all at once.
    accumulator = std::min(accumulator + deltaSeconds, 0.25f);
    while (accumulator >= TICK_SECONDS)
    {
        Tick(state);
        accumulator -= TICK_SECONDS;
    }
    partialTick = accumulator / TICK_SECONDS;
}

// The same spread as vanilla's Level Particles handler: with a count of 0 one particle moves at spread * speed,
// otherwise each of them lands a gaussian away from the position and moves a gaussian times the speed.
void ParticleSystem::Emit(const ParticleSpawn& request, float keep)
{
    if (request.type < 0 || static_cast<size_t>(request.type) >= types.size()) return;
    const Type& type = types[static_cast<size_t>(request.type)];

    if (type.name == "explosion_emitter")
    {
        ParticleSpawn burst = request;
        burst.type = TypeId("explosion");
        burst.count = 1;
        burst.speed = 0.0f;
        burst.spread = glm::vec3(1.5f);
        for (int i = 0; i < 6; i++) Emit(burst, 1.0f);
        return;
    }
    if (type.spriteList.empty()) return;

    auto kept = [&] { return request.alwaysShow || Random() <= keep; };
    if (request.count == 0)
    {
        if (kept()) SpawnOne(request, request.position, request.spread * request.speed);
        return;
    }

    int32_t count = std::min(request.count, MAX_PER_REQUEST);
    for (int32_t i = 0; i < count; i++)
    {
        if (!kept()) continue;
        glm::dvec3 offset(gaussian(rng) * request.spread.x, gaussian(rng) * request.spread.y, gaussian(rng) * request.spread.z);
        glm::vec3 velocity(gaussian(rng) * request.speed, gaussian(rng) * request.speed, gaussian(rng) * request.speed);
        SpawnOne(request, request.position + offset, velocity);
    }
}

void ParticleSystem::SpawnOne(const ParticleSpawn& request, const glm::dvec3& at, const glm::vec3& velocityParameter)
{
    if (particles.size() >= MAX_PARTICLES) return;
    const Type& type = types[static_cast<size_t>(request.type)];
    const ParticleProfile& profile = type.profile;

    Particle particle;
    particle.type = request.type;
    particle.position = particle.previous = at;

    glm::vec3 velocity = velocityParameter;
    if (profile.velocity == Velocity::Generic)
    {
        // Vanilla's base particle: the given velocity plus noise, rescaled to a random speed, with a little lift.
        velocity += glm::vec3(Random() * 2.0f - 1.0f, Random() * 2.0f - 1.0f, Random() * 2.0f - 1.0f) * 0.4f;
        float speed = (Random() + Random() + 1.0f) * 0.15f;
        velocity = velocity / std::max(glm::length(velocity), 1e-5f) * speed * 0.4f;
        velocity.y += 0.1f;
    }
    particle.velocity = velocity;

    particle.lifetime = std::max(1, profile.minLife + static_cast<int>(Random() * static_cast<float>(profile.maxLife - profile.minLife + 1)));
    particle.sizeScale = profile.jitterSize ? 1.0f + Random() : 1.0f;
    particle.tint = profile.tint;
    if (profile.tintFromPacket)
    {
        particle.tint = glm::vec3(static_cast<float>((request.color >> 16) & 0xFF), static_cast<float>((request.color >> 8) & 0xFF),
            static_cast<float>(request.color & 0xFF)) / 255.0f;
        particle.sizeScale *= request.scale;
    }
    if (profile.sprite == ParticleProfile::SpriteChoice::Random)
    {
        particle.spriteIndex = static_cast<uint32_t>(Random() * static_cast<float>(type.spriteList.size())) % static_cast<uint32_t>(type.spriteList.size());
    }
    particles.push_back(particle);
}

void ParticleSystem::Tick(GlobalState* state)
{
    const World* world = state != nullptr ? state->world : nullptr;

    for (size_t i = 0; i < particles.size();)
    {
        Particle& particle = particles[i];
        const ParticleProfile& profile = types[static_cast<size_t>(particle.type)].profile;

        particle.previous = particle.position;
        if (particle.age++ >= particle.lifetime)
        {
            particle = particles.back();
            particles.pop_back();
            continue;
        }

        particle.velocity.y -= GRAVITY_PER_TICK * profile.gravity;

        // One axis at a time, so a particle slides along a wall and lands on the floor.
        particle.onGround = false;
        if (!profile.collides || world == nullptr)
        {
            particle.position += glm::dvec3(particle.velocity);
        }
        else
        {
            auto solid = [&](const glm::dvec3& at)
            {
                return world->GetBlock(static_cast<int>(std::floor(at.x)), static_cast<int>(std::floor(at.y)),
                    static_cast<int>(std::floor(at.z))).hasCollision();
            };
            glm::dvec3 next = particle.position + glm::dvec3(particle.velocity.x, 0.0, 0.0);
            if (solid(next)) particle.velocity.x = 0.0f; else particle.position.x = next.x;
            next = particle.position + glm::dvec3(0.0, particle.velocity.y, 0.0);
            if (solid(next))
            {
                if (particle.velocity.y < 0.0f) particle.onGround = true;
                particle.velocity.y = 0.0f;
            }
            else
            {
                particle.position.y = next.y;
            }
            next = particle.position + glm::dvec3(0.0, 0.0, particle.velocity.z);
            if (solid(next)) particle.velocity.z = 0.0f; else particle.position.z = next.z;
        }

        particle.velocity *= profile.friction;
        if (particle.onGround)
        {
            particle.velocity.x *= 0.7f;
            particle.velocity.z *= 0.7f;
        }

        if (world != nullptr && !profile.bright)
        {
            LightSample light = world->GetLightSample(static_cast<int>(std::floor(particle.position.x)),
                static_cast<int>(std::floor(particle.position.y)), static_cast<int>(std::floor(particle.position.z)));
            particle.skyLight = static_cast<float>(light.sky) / 15.0f;
            particle.blockLight = static_cast<float>(light.block) / 15.0f;
        }
        i++;
    }
}

size_t ParticleSystem::BuildInstances(const glm::vec3& camera, float dayBrightness, ParticleInstance* out, size_t capacity) const
{
    const glm::dvec3 eye(camera);
    size_t count = 0;

    for (const Particle& particle : particles)
    {
        if (count == capacity) break;
        const Type& type = types[static_cast<size_t>(particle.type)];
        const ParticleProfile& profile = type.profile;
        const std::vector<ParticleSprite>& list = type.spriteList;

        float progress = std::clamp((static_cast<float>(particle.age) + partialTick) / static_cast<float>(particle.lifetime), 0.0f, 1.0f);

        // Animated types step through their sprites as they age.
        size_t index = 0;
        if (profile.sprite == ParticleProfile::SpriteChoice::Age)
        {
            index = std::min(list.size() - 1, static_cast<size_t>(particle.age) * (list.size() - 1) / static_cast<size_t>(particle.lifetime));
        }
        else if (profile.sprite == ParticleProfile::SpriteChoice::Random)
        {
            index = particle.spriteIndex;
        }
        const ParticleSprite& sprite = list[index];

        float shrink = 1.0f;
        if (profile.shrink == Shrink::Linear) shrink = 1.0f - progress;
        else if (profile.shrink == Shrink::Flame) shrink = 1.0f - 0.5f * progress * progress;

        // The curve vanilla's lightmap uses, with a floor so nothing goes black.
        float brightness = 1.0f;
        if (!profile.bright)
        {
            float level = std::max(particle.skyLight * dayBrightness, particle.blockLight);
            brightness = 0.08f + 0.92f * (level / (4.0f - 3.0f * level));
        }
        float alpha = profile.fades ? 1.0f - std::clamp((progress - 0.7f) / 0.3f, 0.0f, 1.0f) : 1.0f;

        glm::vec3 at = glm::vec3(glm::mix(particle.previous, particle.position, static_cast<double>(partialTick)) - eye);
        ParticleInstance& instance = out[count++];
        instance.positionSize[0] = at.x;
        instance.positionSize[1] = at.y;
        instance.positionSize[2] = at.z;
        instance.positionSize[3] = profile.size * particle.sizeScale * shrink * sprite.scale * 2.0f; // Vanilla's size is half the edge.
        instance.color[0] = particle.tint.r * brightness;
        instance.color[1] = particle.tint.g * brightness;
        instance.color[2] = particle.tint.b * brightness;
        instance.color[3] = alpha;
        instance.uv[0] = sprite.u0;
        instance.uv[1] = sprite.v0;
        instance.uv[2] = sprite.u1;
        instance.uv[3] = sprite.v1;
        instance.layer = static_cast<float>(sprite.layer);
        instance.roll = 0.0f;
    }
    return count;
}

} // namespace Volcano
