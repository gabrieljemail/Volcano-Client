#pragma once
#ifndef VOLCANO_PARTICLE_SYSTEM_H
#define VOLCANO_PARTICLE_SYSTEM_H

#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "ParticleSprites.hpp"

namespace Volcano {

struct GlobalState;

// What a Spawn() asks for, the way a Level Particles packet does: `count` particles scattered around `position`.
struct ParticleSpawn {
    int32_t type = -1;              // The protocol's particle type id.
    glm::dvec3 position{0.0};
    glm::vec3 spread{0.0f};         // How far from `position` they appear (a gaussian per axis).
    float speed = 0.0f;             // Scales their starting velocity.
    int32_t count = 1;              // 0 spawns one, moving at spread * speed.
    bool alwaysShow = false;        // Shown even with the particle setting turned down.
    uint32_t color = 0xFFFFFFu;     // RGB, for the types that carry one (dust, effects).
    float scale = 1.0f;             // Dust's size.
};

// One particle as ParticlePass draws it. Plain floats, so the layout is exactly this.
struct ParticleInstance {
    float positionSize[4];          // Camera-relative x, y, z, then the edge length in blocks.
    float color[4];
    float uv[4];                    // u0, v0, u1, v1 within the layer.
    float layer;
    float roll;
};

// How one type of particle behaves. Approximates vanilla's particle classes, not a port of them.
struct ParticleProfile {
    enum class Velocity : uint8_t { Generic, Direct };      // Generic is vanilla's base particle: a random burst.
    enum class SpriteChoice : uint8_t { Age, Random, First };
    enum class Shrink : uint8_t { None, Linear, Flame };

    int minLife = 20;               // In ticks.
    int maxLife = 40;
    float size = 0.1f;              // Vanilla's quad size: half the edge, in blocks.
    bool jitterSize = true;         // Each particle 1 to 2 times as big.
    float gravity = 0.0f;           // 1 is vanilla's usual pull; negative rises.
    float friction = 0.98f;
    Velocity velocity = Velocity::Generic;
    SpriteChoice sprite = SpriteChoice::Age;
    Shrink shrink = Shrink::None;
    bool fades = false;
    bool bright = false;            // Ignores the light where it is.
    bool collides = true;
    glm::vec3 tint{1.0f};
    bool tintFromPacket = false;    // Takes the color the packet carries.
};

// The client's particles: spawned by packets and by effects the client makes itself, moved 20 times a second, and
// handed to ParticlePass as instances to draw. Spawn() may be called from any thread; everything else is the
// render thread's.
class ParticleSystem {
public:
    static constexpr size_t MAX_PARTICLES = 16384;

    // Loads the sprites and the type names. Returns false if there are no textures, and then nothing spawns.
    bool Init();

    int32_t TypeId(const std::string& name) const;
    const std::string& TypeName(int32_t id) const;

    // Queued until the next Update().
    void Spawn(const ParticleSpawn& request);

    // Takes the queued spawns and runs the ticks that `deltaSeconds` adds up to.
    void Update(GlobalState* state, float deltaSeconds);

    // Fills `out` with what to draw, positioned relative to `camera`. Returns how many.
    size_t BuildInstances(const glm::vec3& camera, float dayBrightness, ParticleInstance* out, size_t capacity) const;

    const ParticleSprites& Sprites() const { return sprites; }
    size_t Count() const { return particles.size(); }

private:
    struct Type {
        std::string name;
        ParticleProfile profile;
        std::vector<ParticleSprite> spriteList;
    };

    struct Particle {
        int32_t type = 0;
        glm::dvec3 position{0.0};
        glm::dvec3 previous{0.0};
        glm::vec3 velocity{0.0f};
        int age = 0;
        int lifetime = 20;
        float sizeScale = 1.0f;
        glm::vec3 tint{1.0f};
        uint32_t spriteIndex = 0;   // For types that pick one sprite and keep it.
        float skyLight = 1.0f;      // 0 to 1, as of the last tick.
        float blockLight = 0.0f;
        bool onGround = false;
    };

    static constexpr float TICK_SECONDS = 0.05f;

    void Emit(const ParticleSpawn& request, float keep);
    void SpawnOne(const ParticleSpawn& request, const glm::dvec3& at, const glm::vec3& velocityParameter);
    void Tick(GlobalState* state);
    float Random() { return unit(rng); }

    ParticleSprites sprites;
    std::vector<Type> types;
    std::unordered_map<std::string, int32_t> typeIds;
    std::vector<Particle> particles;

    std::mutex pendingMutex;        // Guards pending: spawns come from the network thread.
    std::vector<ParticleSpawn> pending;

    float accumulator = 0.0f;
    float partialTick = 0.0f;
    std::mt19937 rng{ std::random_device{}() };
    std::uniform_real_distribution<float> unit{ 0.0f, 1.0f };
    std::normal_distribution<float> gaussian{ 0.0f, 1.0f };
};

} // namespace Volcano

#endif
