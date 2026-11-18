#include "ClusterGrouping.h"

#include "ClusterBuilder.h"
#include "MeshSource.h"

#include <glm/common.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

void buildClusterGroups(const MeshSource& mesh, ClusterDataset& dataset, uint32_t maxClustersPerGroup)
{
    if (maxClustersPerGroup == 0) {
        throw std::invalid_argument("A group must be allowed at least one cluster.");
    }
    if (mesh.positionIds.size() != mesh.vertices.size() ||
        dataset.clusters.size() >= std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("Invalid mesh or cluster data for grouping.");
    }

    std::unordered_map<uint64_t, std::vector<uint32_t>> edges;
    edges.reserve(dataset.indices.size());
    for (uint32_t id = 0; id < dataset.clusters.size(); ++id) {
        const Cluster& cluster = dataset.clusters[id];
        if (cluster.indexCount == 0 || cluster.indexCount % 3 != 0 ||
            cluster.firstIndex > dataset.indices.size() ||
            cluster.indexCount > dataset.indices.size() - cluster.firstIndex) {
            throw std::invalid_argument("Invalid cluster index range.");
        }

        const size_t end = static_cast<size_t>(cluster.firstIndex) + cluster.indexCount;
        for (size_t first = cluster.firstIndex; first < end; first += 3) {
            uint32_t positions[3];
            for (size_t side = 0; side < 3; ++side) {
                const uint32_t index = dataset.indices[first + side];
                if (index >= mesh.vertices.size()) {
                    throw std::invalid_argument("Cluster vertex index is out of range.");
                }
                positions[side] = mesh.positionIds[index];
            }
            for (size_t side = 0; side < 3; ++side) {
                const uint32_t a = positions[side];
                const uint32_t b = positions[(side + 1) % 3];
                if (a == b) {
                    continue;
                }
                const uint64_t key = (static_cast<uint64_t>(std::min(a, b)) << 32) | std::max(a, b);
                auto& owners = edges[key];
                if (owners.empty() || owners.back() != id) {
                    owners.push_back(id);
                }
            }
        }
    }

    std::vector<std::vector<uint32_t>> neighbors(dataset.clusters.size());
    for (const auto& entry : edges) {
        const auto& owners = entry.second;
        for (size_t i = 0; i < owners.size(); ++i) {
            for (size_t j = i + 1; j < owners.size(); ++j) {
                neighbors[owners[i]].push_back(owners[j]);
                neighbors[owners[j]].push_back(owners[i]);
            }
        }
    }
    for (auto& adjacent : neighbors) {
        std::sort(adjacent.begin(), adjacent.end());
        adjacent.erase(std::unique(adjacent.begin(), adjacent.end()), adjacent.end());
    }

    const uint32_t unassigned = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> groupIds(dataset.clusters.size(), unassigned);
    std::vector<ClusterGroup> groups;
    for (uint32_t seed = 0; seed < dataset.clusters.size(); ++seed) {
        if (groupIds[seed] != unassigned) {
            continue;
        }
        const auto groupId = static_cast<uint32_t>(groups.size());
        groups.push_back({{seed}, dataset.clusters[seed].boundsMin, dataset.clusters[seed].boundsMax});
        ClusterGroup& group = groups.back();
        groupIds[seed] = groupId;

        for (size_t i = 0; i < group.clusters.size() && group.clusters.size() < maxClustersPerGroup; ++i) {
            for (uint32_t neighbor : neighbors[group.clusters[i]]) {
                if (groupIds[neighbor] != unassigned) {
                    continue;
                }
                groupIds[neighbor] = groupId;
                group.clusters.push_back(neighbor);
                group.boundsMin = glm::min(group.boundsMin, dataset.clusters[neighbor].boundsMin);
                group.boundsMax = glm::max(group.boundsMax, dataset.clusters[neighbor].boundsMax);
                if (group.clusters.size() == maxClustersPerGroup) {
                    break;
                }
            }
        }
    }

    dataset.groups = std::move(groups);
    for (size_t id = 0; id < dataset.clusters.size(); ++id) {
        dataset.clusters[id].groupId = groupIds[id];
    }
}
