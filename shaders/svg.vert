#version 450

layout(push_constant) uniform Svg {
    vec4 rect;
    vec4 uv;
    vec4 color;
    vec4 params;
} svg;

layout(location = 0) out vec2 fragUv;

vec2 positions[6] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(1.0, 1.0),
    vec2(0.0, 0.0),
    vec2(1.0, 1.0),
    vec2(0.0, 1.0)
);

void main()
{
    vec2 local = positions[gl_VertexIndex];
    vec2 screen = svg.rect.xy + local * svg.rect.zw;
    vec2 ndc = vec2(
        (screen.x / svg.params.y) * 2.0 - 1.0,
        (screen.y / svg.params.z) * 2.0 - 1.0);

    gl_Position = vec4(ndc, 0.0, 1.0);
    fragUv = svg.uv.xy + local * svg.uv.zw;
}
