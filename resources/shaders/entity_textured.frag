#version 450

layout(set = 1, binding = 0) uniform sampler2D skinTexture;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in float fragHurt;

layout(location = 0) out vec4 outColor;

// Same fixed key light as entity.frag (the placeholder cube's shader) —
// entities don't carry chunk sky/block light values yet, see that shader's
// own comment.
const vec3 LIGHT_DIR = vec3(0.3838, 0.7845, 0.4885); // normalize(vec3(0.3, 1.0, 0.5))

void main() {
    vec4 texColor = texture(skinTexture, fragUV);
    if (texColor.a < 0.5) discard; // Skin sheets carry unused/transparent regions outside each box's own UV rect.

    float brightness = 0.4 + 0.6 * max(dot(normalize(fragNormal), LIGHT_DIR), 0.0);
    vec3 shaded = texColor.rgb * brightness;

    // Tick-invulnerability hit flash, same convention as entity.frag.
    vec3 finalColor = mix(shaded, vec3(1.0, 0.0, 0.0), fragHurt);
    outColor = vec4(finalColor, 1.0);
}
