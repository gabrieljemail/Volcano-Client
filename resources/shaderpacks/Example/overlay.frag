#version 450

// Example shader pack overlay: a soft vignette that deepens and turns blue
// at night, fading out entirely at noon. Alpha-blended over the world
// (before the GUI), so alpha 0 leaves a pixel untouched.

layout(push_constant) uniform Overlay {
    vec4 viewport;
    vec4 sky;
    vec4 camera;
} overlay;

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 centered = fragUV - 0.5;
    centered.x *= overlay.viewport.x / max(overlay.viewport.y, 1.0);

    float vignette = smoothstep(0.35, 0.95, length(centered));
    float night = 1.0 - overlay.sky.a;

    vec3 tint = mix(vec3(0.0), vec3(0.02, 0.03, 0.10), night);
    outColor = vec4(tint, vignette * mix(0.25, 0.6, night));
}
