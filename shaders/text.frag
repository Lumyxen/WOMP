#version 450

layout(binding = 0) uniform sampler2D textAtlas;

layout(push_constant) uniform Glyph {
    vec4 rect;
    vec4 uv;
    vec4 color;
    vec4 atlas;
} glyph;

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 color;

void main()
{
    float alpha = texture(textAtlas, fragUv).r;
    color = glyph.color;
    color.a *= alpha;

    if (color.a <= 0.001) {
        discard;
    }
}
