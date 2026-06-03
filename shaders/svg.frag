#version 450

layout(binding = 0) uniform sampler2D svgAtlas;

layout(push_constant) uniform Svg {
    vec4 rect;
    vec4 uv;
    vec4 color;
    vec4 params;
} svg;

layout(location = 0) in vec2 fragUv;
layout(location = 0) out vec4 color;

void main()
{
    vec4 sampleColor = texture(svgAtlas, fragUv);

    if (svg.params.x > 1.5) {
        color = svg.color;
        color.a *= sampleColor.a;
    } else if (svg.params.x > 0.5) {
        float distanceAlpha = sampleColor.a;
        float width = max(fwidth(distanceAlpha), 0.001);
        float alpha = smoothstep(0.5 - width, 0.5 + width, distanceAlpha);
        color = svg.color;
        color.a *= alpha;
    } else {
        color = sampleColor;
        color.a *= svg.color.a;
    }

    if (color.a <= 0.001) {
        discard;
    }
}
