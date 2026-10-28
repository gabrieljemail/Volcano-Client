#version 450

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 offset;
} push;

layout(location = 0) in float fragShade;
layout(location = 1) in float fragDistance;

layout(location = 0) out vec4 outColor;

void main() {
    // Fade out over the last quarter of the cloud radius.
    float fade = 1.0 - smoothstep(push.offset.w * 0.75, push.offset.w, fragDistance);
    outColor = vec4(push.color.rgb * fragShade, push.color.a * fade);
}
