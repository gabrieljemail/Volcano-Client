#version 450

// Two modes on the same cube: the title panorama (mode 0), or the in-game
// sky (mode 1) — a zenith-to-horizon gradient with the sun and moon on
// opposite sides and a sunrise/sunset glow.
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 skyTop;   // rgb, w = mode
    vec4 horizon;  // rgb, w = moon phase (0-7)
    vec4 sunDir;   // xyz
} push;

layout(set = 0, binding = 0) uniform samplerCube panorama;
layout(set = 0, binding = 1) uniform sampler2DArray celestial; // layer 0 = sun, 1-8 = moon phases

layout(location = 0) in vec3 texCoord;
layout(location = 0) out vec4 outColor;

// Vanilla draws them 30 units wide at a distance of 100.
const float CELESTIAL_HALF_SIZE = 0.15;
const vec3 SUNSET_COLOR = vec3(1.0, 0.27, 0.05); // linear (sRGB ~ 1.0, 0.55, 0.25)

// Square billboard centered on `axis`, sampled from layer `layer`.
vec4 Celestial(vec3 dir, vec3 axis, float layer)
{
    float facing = dot(dir, axis);
    if (facing <= 0.0) return vec4(0.0);

    vec3 onPlane = dir / facing - axis;
    vec3 across = vec3(0.0, 0.0, 1.0);          // the sun's path is in the XY plane
    vec3 along = cross(across, axis);
    vec2 uv = vec2(dot(onPlane, along), dot(onPlane, across)) / (CELESTIAL_HALF_SIZE * 2.0) + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(0.0);
    return texture(celestial, vec3(uv, layer));
}

void main()
{
    if (push.skyTop.w < 0.5)
    {
        outColor = vec4(texture(panorama, texCoord).rgb, 1.0);
        return;
    }

    vec3 dir = normalize(texCoord);
    vec3 color = mix(push.horizon.rgb, push.skyTop.rgb, smoothstep(0.0, 0.5, dir.y));

    // Sunrise/sunset: an orange glow on the sun's side, near the horizon.
    vec3 sun = normalize(push.sunDir.xyz);
    float sunNearHorizon = 1.0 - smoothstep(0.0, 0.35, abs(sun.y));
    float towardSun = pow(max(dot(normalize(vec3(dir.x, 0.0, dir.z) + 1e-5), normalize(vec3(sun.x, 0.0, sun.z) + 1e-5)), 0.0), 3.0);
    float nearHorizon = 1.0 - smoothstep(0.0, 0.45, abs(dir.y));
    color = mix(color, SUNSET_COLOR, sunNearHorizon * towardSun * nearHorizon * 0.8);

    // Both are drawn additively, like vanilla (their textures are on black).
    vec4 sunTexel = Celestial(dir, sun, 0.0);
    vec4 moonTexel = Celestial(dir, -sun, 1.0 + push.horizon.w);
    color += sunTexel.rgb * sunTexel.a + moonTexel.rgb * moonTexel.a;

    outColor = vec4(color, 1.0);
}
