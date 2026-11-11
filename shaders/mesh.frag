#version 450

layout(location = 0) in vec3 surfaceNormal;
layout(location = 0) out vec4 color;

layout(push_constant) uniform DrawParams {
    layout(offset = 64) vec4 baseColor;
} drawParams;

void main()
{
    vec3 lightDirection = normalize(vec3(0.6, 0.8, 0.5));
    float diffuse = max(dot(normalize(surfaceNormal), lightDirection), 0.0);
    color = vec4(drawParams.baseColor.rgb * (0.2 + 0.8 * diffuse), 1.0);
}
