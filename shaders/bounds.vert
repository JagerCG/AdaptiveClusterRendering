#version 450

struct ClusterDesc {
    vec4 boundsMin;
    vec4 boundsMax;
    uvec4 indexRange;
};
layout(std430, set = 0, binding = 0) readonly buffer ClusterBuffer {
    ClusterDesc clusters[];
};
layout(push_constant) uniform DrawParams { mat4 viewProjection; } drawParams;
layout(location = 0) out vec3 lineColor;

// 12 edges, two endpoints each; corner xyz corresponds to the three low bits.
const int corners[24] = int[](0,1, 2,3, 4,5, 6,7, 0,2, 1,3, 4,6, 5,7, 0,4, 1,5, 2,6, 3,7);
void main()
{
    ClusterDesc cluster = clusters[gl_InstanceIndex];
    int corner = corners[gl_VertexIndex];
    vec3 p = mix(cluster.boundsMin.xyz, cluster.boundsMax.xyz,
                 vec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1));
    gl_Position = drawParams.viewProjection * vec4(p, 1);
    // Stable IDs are read from the same SSBO as the bounds.
    float hue = float((cluster.indexRange.z * 2654435761u) >> 8) / 16777216.0;
    lineColor = 0.4 + 0.6 * clamp(abs(fract(hue + vec3(0, 2.0/3.0, 1.0/3.0)) * 6 - 3) - 1, 0, 1);
}
