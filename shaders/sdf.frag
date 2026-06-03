#version 450

layout(push_constant) uniform Shape {
    vec4 rect;
    vec4 fill;
    vec4 stroke;
    vec4 params;
    vec4 extra;
} shape;

layout(location = 0) out vec4 color;

float roundedRectSdf(vec2 point, vec2 size, float radius)
{
    vec2 halfSize = size * 0.5;
    float clampedRadius = clamp(radius, 0.0, min(halfSize.x, halfSize.y));
    vec2 q = abs(point) - halfSize + vec2(clampedRadius);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - clampedRadius;
}

float circleSdf(vec2 point, float radius)
{
    return length(point) - max(radius, 0.0);
}

float lineSdf(vec2 point, vec2 start, vec2 end, float thickness)
{
    vec2 segment = end - start;
    float segmentLengthSq = dot(segment, segment);
    float t = segmentLengthSq > 0.0 ? clamp(dot(point - start, segment) / segmentLengthSq, 0.0, 1.0) : 0.0;
    return length(point - (start + segment * t)) - max(thickness * 0.5, 0.0);
}

float quadSdf(vec2 point, vec2 origin, vec2 size)
{
    vec2 center = origin + size * 0.5;
    vec2 halfSize = abs(size) * 0.5;
    vec2 q = abs(point - center) - halfSize;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
}

float signedTriangleArea(vec2 a, vec2 b, vec2 c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float segmentDistance(vec2 point, vec2 start, vec2 end)
{
    vec2 segment = end - start;
    float segmentLengthSq = dot(segment, segment);
    float t = segmentLengthSq > 0.0 ? clamp(dot(point - start, segment) / segmentLengthSq, 0.0, 1.0) : 0.0;
    return length(point - (start + segment * t));
}

float triangleSdf(vec2 point, vec2 a, vec2 b, vec2 c)
{
    float winding = sign(signedTriangleArea(a, b, c));
    winding = winding == 0.0 ? 1.0 : winding;

    float d0 = winding * signedTriangleArea(a, b, point);
    float d1 = winding * signedTriangleArea(b, c, point);
    float d2 = winding * signedTriangleArea(c, a, point);
    float minEdge = min(d0, min(d1, d2));
    float edgeDistance = min(
        segmentDistance(point, a, b),
        min(segmentDistance(point, b, c), segmentDistance(point, c, a)));

    return minEdge >= 0.0 ? -edgeDistance : edgeDistance;
}

vec4 compose(float distanceToEdge)
{
    float edgeSoftness = max(shape.params.w, 0.5);
    float strokeWidth = max(shape.params.y, 0.0);
    float fillAlpha = 1.0 - smoothstep(-edgeSoftness, edgeSoftness, distanceToEdge);

    vec4 result = shape.fill;
    result.a *= fillAlpha;

    if (strokeWidth > 0.0) {
        float strokeDistance = abs(distanceToEdge + strokeWidth * 0.5) - strokeWidth * 0.5;
        float strokeAlpha = 1.0 - smoothstep(-edgeSoftness, edgeSoftness, strokeDistance);
        vec4 strokeColor = shape.stroke;
        strokeColor.a *= strokeAlpha;
        result = mix(result, strokeColor, strokeColor.a);
        result.a = max(result.a, strokeColor.a);
    }

    return result;
}

void main()
{
    int kind = int(shape.params.z + 0.5);
    float distanceToEdge = 0.0;

    if (kind == 1) {
        distanceToEdge = circleSdf(gl_FragCoord.xy - shape.rect.xy, shape.params.x);
    } else if (kind == 2) {
        distanceToEdge = lineSdf(gl_FragCoord.xy, shape.rect.xy, shape.rect.zw, shape.params.x);
    } else if (kind == 3) {
        distanceToEdge = quadSdf(gl_FragCoord.xy, shape.rect.xy, shape.rect.zw);
    } else if (kind == 4) {
        distanceToEdge = triangleSdf(gl_FragCoord.xy, shape.rect.xy, shape.rect.zw, shape.extra.xy);
    } else {
        vec2 center = shape.rect.xy + shape.rect.zw * 0.5;
        distanceToEdge = roundedRectSdf(gl_FragCoord.xy - center, shape.rect.zw, shape.params.x);
    }

    color = compose(distanceToEdge);
    if (color.a <= 0.001) {
        discard;
    }
}
