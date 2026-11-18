#pragma once

#include "ClusterBuilder.h"
#include <cstddef>
#include <type_traits>

struct SceneData;
constexpr uint32_t kNoParent = UINT32_MAX;

// Indices and cluster IDs are absolute in [fine data | coarse data].
// These fixed-size ACR records are also uploaded as std430 data.
struct alignas(16) AcrLevelRecord {
    uint32_t firstNode, nodeCount, firstCluster, clusterCount;
    uint32_t firstIndex, indexCount;
    float buildMs;
    uint32_t maxDiameterPixels = 0; // zero for regular levels; whole-object footprint limit for distant proxies
};

struct alignas(16) AcrNodeRecord {
    glm::vec3 boundsMin;
    float error; // conservative, cumulative displacement in model units
    glm::vec3 boundsMax;
    uint32_t parent;
    uint32_t firstCluster, clusterCount, firstChild, childCount;
};
static_assert(sizeof(AcrLevelRecord) == 32 && offsetof(AcrLevelRecord, buildMs) == 24);
static_assert(sizeof(AcrNodeRecord) == 48 && offsetof(AcrNodeRecord, error) == 12 && offsetof(AcrNodeRecord, parent) == 28 &&
              offsetof(AcrNodeRecord, firstCluster) == 32);
static_assert(std::is_trivially_copyable_v<AcrNodeRecord> && std::is_trivially_copyable_v<AcrLevelRecord>);

struct MeshHierarchy {
    ClusterDataset coarse; // shares the original vertex/attribute array
    std::vector<AcrLevelRecord> levels;
    std::vector<AcrNodeRecord> nodes;
    std::vector<uint32_t> children; // absolute node IDs
};

// Legacy and hand-authored scenes have an implicit level zero.
AcrLevelRecord sceneLevel(const SceneData& scene, uint32_t level);
const Cluster& sceneCluster(const SceneData& scene, uint32_t id);
void validateMeshHierarchy(const SceneData& scene);
