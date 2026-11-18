#ifndef LOD_DATA_GLSL
#define LOD_DATA_GLSL

struct LodNode {
    vec4 minimumError;
    vec4 maximum;
    uvec4 links;
};

struct ClusterInfo {
    uvec4 geometry;
    uvec4 colors;
};

#endif
