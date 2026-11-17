#version 450
layout(location = 0) in vec3 surfaceNormal;
layout(location = 1) in vec3 worldPosition;
layout(location = 0) out vec4 color;
layout(set = 1, binding = 0) uniform sampler2D shadowMap;
layout(std140, set = 1, binding = 1) uniform Lighting {
    mat4 lightViewProjection;
    vec4 lightDirection;
} lighting;
layout(push_constant) uniform DrawParams {
    layout(offset = 64) vec4 baseColor;
    layout(offset = 80) float boundsPadding;
    layout(offset = 84) uint displayMode;
    layout(offset = 88) uint shadows;
    layout(offset = 92) float shadowBias;
} params;
void main()
{
    float diffuse = max(dot(normalize(surfaceNormal), lighting.lightDirection.xyz), 0);
    float visibility = 1;
    if (params.shadows != 0u) {
        vec4 clip = lighting.lightViewProjection * vec4(worldPosition, 1);
        vec3 p = clip.xyz / clip.w;
        vec2 uv = p.xy * .5 + .5;
        if (p.z >= 0 && p.z <= 1 && all(greaterThanEqual(uv, vec2(0))) && all(lessThanEqual(uv, vec2(1)))) {
            visibility = 0;
            vec2 texel = 1.0 / vec2(textureSize(shadowMap, 0));
            for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x)
                visibility += (p.z - params.shadowBias <= texture(shadowMap, uv + vec2(x, y) * texel).r) ? 1 : 0;
            visibility /= 9;
        }
    }
    vec3 shaded = params.baseColor.rgb * (.2 + .8 * diffuse * visibility);
    color = vec4(shaded, 1);
}
