#version 450

layout(set = 0, binding = 0) uniform sampler2D pincelTexture;

layout(push_constant) uniform PushConstants
{
    vec4 viewport;
    vec4 rect;
    vec4 borders;
    vec4 borderColor;
    float radius;
    int kind;
} pc;

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec4 fragColor;
layout(location = 0) out vec4 outColor;

float RoundedDistance(vec2 point, vec2 size, float cornerRadius)
{
    vec2 q = abs(point - size * 0.5) - (size * 0.5 - vec2(cornerRadius));
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - cornerRadius;
}

float Coverage(float distance)
{
    float antialias = max(fwidth(distance), 0.0001);
    return 1.0 - smoothstep(-antialias, antialias, distance);
}

void main()
{
    if (pc.kind == 0)
    {
        vec2 local = gl_FragCoord.xy - pc.rect.xy;
        float outer = Coverage(RoundedDistance(local, pc.rect.zw, max(pc.radius, 0.0)));
        vec2 inset = vec2(pc.borders.w, pc.borders.x);
        vec2 innerSize = pc.rect.zw - vec2(pc.borders.y + pc.borders.w, pc.borders.x + pc.borders.z);
        float inner = 0.0;
        if (innerSize.x > 0.0 && innerSize.y > 0.0)
        {
            float insetRadius = max(0.0, pc.radius - max(max(pc.borders.x, pc.borders.y), max(pc.borders.z, pc.borders.w)));
            inner = Coverage(RoundedDistance(local - inset, innerSize, insetRadius));
        }
        vec4 fillColor = vec4(fragColor.rgb * fragColor.a, fragColor.a);
        vec4 edgeColor = vec4(pc.borderColor.rgb * pc.borderColor.a, pc.borderColor.a);
        outColor = fillColor * inner + edgeColor * max(outer - inner, 0.0);
    }
    else if (pc.kind == 1)
    {
        vec4 sampleColor = texture(pincelTexture, fragUV);
        float mask = pc.radius > 0.0
            ? Coverage(RoundedDistance(gl_FragCoord.xy - pc.rect.xy, pc.rect.zw, pc.radius))
            : 1.0;
        float alpha = sampleColor.a * mask;
        outColor = vec4(sampleColor.rgb * alpha, alpha);
    }
    else
    {
        float alpha = texture(pincelTexture, fragUV).r * fragColor.a;
        outColor = vec4(fragColor.rgb * alpha, alpha);
    }
}
