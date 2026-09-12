#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in float fragHurt;

layout(location = 0) out vec4 outColor;

// No lighting system reaches entities yet (they don't carry chunk sky/block
// light values), so a fixed key light gives the placeholder box some shape
// instead of flat-shading it into a silhouette.
const vec3 LIGHT_DIR = vec3(0.3838, 0.7845, 0.4885); // normalize(vec3(0.3, 1.0, 0.5))

void main() {
    float brightness = 0.4 + 0.6 * max(dot(normalize(fragNormal), LIGHT_DIR), 0.0);
    vec3 shaded = fragColor.rgb * brightness;

    // Tick-invulnerability hit flash: entity-only color layer per the
    // entity renderer plan, driven by per-instance hurtAmount (0 = normal,
    // 1 = fully red) rather than a separate render pass.
    vec3 finalColor = mix(shaded, vec3(1.0, 0.0, 0.0), fragHurt);
    outColor = vec4(finalColor, fragColor.a);
}
