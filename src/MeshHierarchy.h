#pragma once

#include "ClusterBuilder.h"
#include <cstddef>
#include <type_traits>

struct SceneData;
constexpr uint32_t kNoParent = UINT32_MAX;
constexpr uint32_t kAdaptiveGeometryLevel = UINT32_MAX; // runtime cut across resident levels

// Indices and cluster IDs are absolute in [fine data | coarse data].
// Levels and nodes are also the std430 storage records consumed by the GPU.
struct alignas(16) LodLevel {
    uint32_t firstNode, nodeCount, firstCluster, clusterCount;
    uint32_t firstIndex, indexCount;
    float buildMs;
    uint32_t maxDiameterPixels = 0; // zero for regular levels; whole-object footprint limit for distant proxies
};

struct alignas(16) LodNode {
    glm::vec3 boundsMin;
    float error; // conservative, cumulative displacement in model units
    glm::vec3 boundsMax;
    uint32_t parent;
    uint32_t firstCluster, clusterCount, firstChild, childCount;
};
static_assert(sizeof(LodLevel) == 32 && offsetof(LodLevel, buildMs) == 24);
static_assert(sizeof(LodNode) == 48 && offsetof(LodNode, error) == 12 && offsetof(LodNode, parent) == 28 &&
              offsetof(LodNode, firstCluster) == 32);
static_assert(std::is_trivially_copyable_v<LodNode> && std::is_trivially_copyable_v<LodLevel>);

struct MeshHierarchy {
    ClusterDataset coarse; // shares the original vertex/attribute array
    std::vector<LodLevel> levels;
    std::vector<LodNode> nodes;
    std::vector<uint32_t> children; // absolute node IDs
};

// Legacy and hand-authored scenes have an implicit level zero.
LodLevel sceneLevel(const SceneData& scene, uint32_t level);
const Cluster& sceneCluster(const SceneData& scene, uint32_t id);
void buildMeshHierarchy(SceneData& scene, float fineBuildMs);
void validateMeshHierarchy(const SceneData& scene);
