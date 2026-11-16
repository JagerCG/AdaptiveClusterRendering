#version 450

layout(location = 0) in vec3 surfaceNormal;
layout(location = 1) in vec3 worldPosition;
layout(location = 0) out vec4 color;
layout(set = 1, binding = 0) uniform sampler2D shadowMap;

layout(push_constant) uniform DrawParams {
    layout(offset = 64) vec4 baseColor;
    layout(offset = 80) uint drawMode;
    layout(offset = 84) uint shadowEnabled;
    layout(offset = 88) uint shadowMapView;
} drawParams;

void main()
{
    vec3 base = drawParams.baseColor.rgb;
    vec3 lightDirection = normalize(vec3(0.6, 0.8, 0.5));
    float diffuse = max(dot(normalize(surfaceNormal), lightDirection), 0.0);
    vec3 shaded = base * (0.2 + 0.8 * diffuse);
    if (drawParams.shadowEnabled == 1u) {
        vec2 shadowUv = vec2(worldPosition.x * 0.5 + 0.5, 1.0 - (worldPosition.z * 0.5 + 0.5));
        float shadowDepth = texture(shadowMap, shadowUv).r;
        float receiverDepth = clamp((worldPosition.y + 2.0) / 6.0, 0.0, 1.0);
        float shadow = shadowDepth > receiverDepth + 0.01 ? 1.0 : 0.35;
        shaded *= shadow;
    }
    if (drawParams.shadowMapView == 1u) {
        vec2 shadowUv = vec2(worldPosition.x * 0.5 + 0.5, 1.0 - (worldPosition.z * 0.5 + 0.5));
        shaded = vec3(texture(shadowMap, shadowUv).rrr);
    }
    if (drawParams.drawMode == 1u) {
        shaded = vec3(gl_FragCoord.z);
    }
    color = vec4(shaded, 1.0);
}
