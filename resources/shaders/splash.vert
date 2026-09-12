#version 450

// Letterbox scale (1.0 on the axis the image fills edge-to-edge, <1.0 on
// the other axis to preserve its aspect ratio) — computed on the CPU from
// the loaded image's size vs. the current swapchain extent, see
// SplashScreen::Show().
layout(push_constant) uniform PushConstants {
    vec2 scale;
} pc;

layout(location = 0) out vec2 fragUV;

// No vertex buffer: a triangle-strip quad generated straight from
// gl_VertexIndex, same trick RenderThread's fullscreen passes could use if
// they needed one — there's exactly one draw call ever issued through this
// pipeline, so a dedicated vertex buffer would be pure overhead.
void main() {
    vec2 positions[4] = vec2[](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0)
    );
    vec2 uvs[4] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(1.0, 1.0)
    );

    gl_Position = vec4(positions[gl_VertexIndex] * pc.scale, 0.0, 1.0);
    fragUV = uvs[gl_VertexIndex];
}
