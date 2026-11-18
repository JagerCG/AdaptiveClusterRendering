#pragma once

#include "ClusterBuilder.h"
#include "MeshSource.h"

struct LodNode {
    uint32_t level = 0;
    uint32_t firstCluster = 0;
    uint32_t clusterCount = 0;
    std::vector<uint32_t> children;
    glm::vec3 boundsMin;
    glm::vec3 boundsMax;
    float error = 0.0f;
};

struct LodLevel {
    MeshSource mesh;
    ClusterDataset dataset;
    std::vector<uint32_t> nodes;
    float maxError = 0.0f;
};

struct LodHierarchy {
    std::vector<LodLevel> levels;
    std::vector<LodNode> nodes;
    std::vector<uint32_t> roots;
};

LodHierarchy buildLodHierarchy(MeshSource mesh, float triangleRatio = 0.5f, uint32_t maxLevels = 8);
