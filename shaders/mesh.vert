#version 450
#extension GL_GOOGLE_include_directive : require

#include "lod_data.glsl"

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 0) out vec3 surfaceNormal;
layout(location = 1) flat out vec4 surfaceColor;

layout(set = 0, binding = 1, std430) readonly buffer ClusterData { ClusterInfo clusters[]; };

layout(push_constant) uniform DrawParams {
    mat4 viewProjection;
    uvec4 options;
} drawParams;

vec4 debugColor(uint id)
{
    uint hueBits = id * 2654435761u;
    float hue = float(hueBits >> 8) / 16777216.0;
    vec3 cycle = fract(vec3(hue) + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0));
    vec3 rgb = clamp(abs(cycle * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    return vec4(0.22 + 0.73 * rgb, 1.0);
}

void main()
{
    gl_Position = drawParams.viewProjection * vec4(position, 1.0);
    surfaceNormal = normal;
    ClusterInfo cluster = clusters[uint(gl_InstanceIndex)];
    uint mode = drawParams.options.x;
    uint id = mode == 1u ? cluster.colors.x : (mode == 2u ? cluster.colors.y : cluster.geometry.w);
    surfaceColor = mode == 0u ? vec4(0.24, 0.54, 0.84, 1.0) : debugColor(id);
}
