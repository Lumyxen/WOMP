#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main()
{
    vec3 top = vec3(0.08, 0.10, 0.15);
    vec3 bottom = vec3(0.66, 0.20, 0.12);
    vec3 accent = vec3(0.10, 0.42, 0.58) * smoothstep(0.15, 0.85, uv.x);

    color = vec4(mix(bottom, top, uv.y) + accent * 0.22, 1.0);
}
