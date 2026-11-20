#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform PushConstants
{
    vec4 viewport;
    vec4 rect;
    vec4 borders;
    vec4 borderColor;
    float radius;
    int kind;
} pc;

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec4 fragColor;

void main()
{
    vec2 position = vec2(inPosition.x * 2.0 / pc.viewport.x - 1.0,
        inPosition.y * 2.0 / pc.viewport.y - 1.0);
    gl_Position = vec4(position, 0.0, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
