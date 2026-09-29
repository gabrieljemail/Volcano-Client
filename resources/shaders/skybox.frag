#version 450

layout(set = 0, binding = 0) uniform samplerCube panorama;

layout(location = 0) in vec3 texCoord;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(texture(panorama, texCoord).rgb, 1.0);
}
