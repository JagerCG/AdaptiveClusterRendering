#pragma once

#include "ClusterBuilder.h"

#include <glm/vec4.hpp>
#include <cstddef>
#include <type_traits>
#include <vector>

// std430, set 0 / binding 0. Do not upload the compiler-dependent CPU Cluster.
struct alignas(16) GpuClusterDesc {
    glm::vec4 boundsMin;
    glm::vec4 boundsMax;
    glm::uvec4 indexRange; // firstIndex, indexCount, stable ID, reserved (= 0)
};
static_assert(std::is_standard_layout_v<GpuClusterDesc> && std::is_trivially_copyable_v<GpuClusterDesc>);
static_assert(sizeof(GpuClusterDesc) == 48 && alignof(GpuClusterDesc) == 16);
static_assert(offsetof(GpuClusterDesc, boundsMax) == 16 && offsetof(GpuClusterDesc, indexRange) == 32);

inline std::vector<GpuClusterDesc> packClusters(const ClusterDataset& dataset)
{
    std::vector<GpuClusterDesc> result;
    result.reserve(dataset.clusters.size());
    for (size_t id = 0; id < dataset.clusters.size(); ++id) {
        const auto& c = dataset.clusters[id];
        result.push_back({glm::vec4(c.boundsMin, 0.0f), glm::vec4(c.boundsMax, 0.0f),
                          {c.firstIndex, c.indexCount, static_cast<uint32_t>(id), 0}});
    }
    return result;
}
