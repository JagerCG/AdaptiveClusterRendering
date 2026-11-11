#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

struct MeshSource;

struct Cluster {
    uint32_t firstIndex;
    uint32_t indexCount;
    glm::vec3 boundsMin;
    glm::vec3 boundsMax;
};

struct ClusterDataset {
    std::vector<uint32_t> indices;
    // A cluster's ID is its position in this list.
    std::vector<Cluster> clusters;
};

ClusterDataset buildClusters(const MeshSource& mesh, uint32_t maxTrianglesPerCluster = 64);
