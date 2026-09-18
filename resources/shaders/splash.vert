#version 450

// Crop scale (1.0 on the axis the image fills edge-to-edge, <1.0 on the
// axis that needs cropping to avoid letterbox bars while still preserving
// the image's aspect ratio) — computed on the CPU from the loaded image's
// size vs. the current swapchain extent, see SplashScreen::Show(). Applied
// to the sampled UV rect (cropping it toward its center), not to the quad's
// position, so the quad always covers the full window.
layout(push_constant) uniform PushConstants {
    vec2 uvScale;
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

    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vec2 uvOffset = (1.0 - pc.uvScale) * 0.5;
    fragUV = uvs[gl_VertexIndex] * pc.uvScale + uvOffset;
}
