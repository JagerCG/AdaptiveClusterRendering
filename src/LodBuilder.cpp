#include "LodBuilder.h"

#include "ClusterGrouping.h"
#include "MeshSimplifier.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

std::vector<std::vector<uint32_t>> groupNodes(LodLevel& level, const std::vector<LodNode>& nodes)
{
    ClusterDataset regions;
    regions.indices = level.dataset.indices;
    for (uint32_t id : level.nodes) {
        const auto& node = nodes[id];
        const auto& first = level.dataset.clusters[node.firstCluster];
        const auto& last = level.dataset.clusters[node.firstCluster + node.clusterCount - 1];
        regions.clusters.push_back({first.firstIndex, last.firstIndex + last.indexCount - first.firstIndex,
                                     node.boundsMin, node.boundsMax});
    }
    buildClusterGroups(level.mesh, regions);
    level.dataset.groups.clear();
    std::vector<std::vector<uint32_t>> result;
    for (const auto& regionGroup : regions.groups) {
        ClusterGroup group{{}, regionGroup.boundsMin, regionGroup.boundsMax};
        std::vector<uint32_t> children;
        const auto groupId = static_cast<uint32_t>(result.size());
        for (uint32_t region : regionGroup.clusters) {
            const uint32_t id = level.nodes[region];
            children.push_back(id);
            const auto& node = nodes[id];
            for (uint32_t i = 0; i < node.clusterCount; ++i) {
                const uint32_t cluster = node.firstCluster + i;
                group.clusters.push_back(cluster);
                level.dataset.clusters[cluster].groupId = groupId;
            }
        }
        result.push_back(std::move(children));
        level.dataset.groups.push_back(std::move(group));
    }
    return result;
}

std::vector<std::vector<uint32_t>> sharedPositions(const LodLevel& level)
{
    std::unordered_map<uint32_t, uint32_t> owners;
    std::vector<std::vector<uint32_t>> locked(level.dataset.groups.size());
    for (uint32_t groupId = 0; groupId < level.dataset.groups.size(); ++groupId) {
        for (uint32_t id : level.dataset.groups[groupId].clusters) {
            const auto& cluster = level.dataset.clusters[id];
            const size_t end = size_t(cluster.firstIndex) + cluster.indexCount;
            for (size_t i = cluster.firstIndex; i < end; ++i) {
                const uint32_t position = level.mesh.positionIds[level.dataset.indices[i]];
                const auto [entry, inserted] = owners.try_emplace(position, groupId);
                if (!inserted && entry->second != groupId) {
                    locked[entry->second].push_back(position);
                    locked[groupId].push_back(position);
                }
            }
        }
    }
    for (auto& positions : locked) {
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    }
    return locked;
}

MeshSource extractGroup(const LodLevel& level, const ClusterGroup& group)
{
    MeshSource mesh;
    std::unordered_map<uint32_t, uint32_t> remap;
    for (uint32_t id : group.clusters) {
        const auto& cluster = level.dataset.clusters[id];
        const size_t end = size_t(cluster.firstIndex) + cluster.indexCount;
        for (size_t i = cluster.firstIndex; i < end; ++i) {
            const uint32_t vertex = level.dataset.indices[i];
            const auto [entry, inserted] = remap.try_emplace(vertex, uint32_t(mesh.vertices.size()));
            if (inserted) {
                mesh.vertices.push_back(level.mesh.vertices[vertex]);
                mesh.positionIds.push_back(level.mesh.positionIds[vertex]);
            }
            mesh.indices.push_back(entry->second);
        }
    }
    return mesh;
}

bool hasSurface(const MeshSource& mesh)
{
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const glm::dvec3 a(mesh.vertices[mesh.indices[i]].position);
        const glm::dvec3 b(mesh.vertices[mesh.indices[i + 1]].position);
        const glm::dvec3 c(mesh.vertices[mesh.indices[i + 2]].position);
        if (glm::length(glm::cross(b - a, c - a)) > 0.0) return true;
    }
    return false;
}

void appendMesh(LodLevel& level, const MeshSource& mesh, const ClusterDataset& dataset)
{
    const auto limit = std::numeric_limits<uint32_t>::max();
    if (mesh.vertices.size() > limit - level.mesh.vertices.size() ||
        dataset.indices.size() > limit - level.dataset.indices.size()) {
        throw std::overflow_error("LOD geometry exceeds 32-bit index limits.");
    }
    const auto vertexOffset = static_cast<uint32_t>(level.mesh.vertices.size());
    const auto indexOffset = static_cast<uint32_t>(level.dataset.indices.size());
    level.mesh.vertices.insert(level.mesh.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
    level.mesh.positionIds.insert(level.mesh.positionIds.end(), mesh.positionIds.begin(), mesh.positionIds.end());
    for (uint32_t index : dataset.indices) level.dataset.indices.push_back(vertexOffset + index);
    for (auto cluster : dataset.clusters) {
        cluster.firstIndex += indexOffset;
        level.dataset.clusters.push_back(cluster);
    }
}

}

LodHierarchy buildLodHierarchy(MeshSource mesh, float triangleRatio, uint32_t maxLevels)
{
    if (maxLevels == 0 || !std::isfinite(triangleRatio) || triangleRatio <= 0.0f || triangleRatio > 1.0f) {
        throw std::invalid_argument("Invalid LOD level count or triangle ratio.");
    }
    LodHierarchy hierarchy;
    LodLevel base;
    base.mesh = std::move(mesh);
    base.dataset = buildClusters(base.mesh);
    buildClusterGroups(base.mesh, base.dataset);
    for (uint32_t id = 0; id < base.dataset.clusters.size(); ++id) {
        const auto& cluster = base.dataset.clusters[id];
        base.nodes.push_back(id);
        hierarchy.nodes.push_back({0, id, 1, {}, cluster.boundsMin, cluster.boundsMax, 0.0f});
    }
    hierarchy.levels.push_back(std::move(base));

    for (uint32_t levelId = 1; levelId < maxLevels; ++levelId) {
        auto& current = hierarchy.levels.back();
        const auto groups = groupNodes(current, hierarchy.nodes);
        const auto locked = sharedPositions(current);
        LodLevel next;
        std::vector<LodNode> parents;
        for (size_t groupId = 0; groupId < groups.size(); ++groupId) {
            const auto& group = current.dataset.groups[groupId];
            auto input = extractGroup(current, group);
            SimplificationResult simplified;
            if (hasSurface(input)) {
                simplified = simplifyMesh(input, triangleRatio, locked[groupId]);
            }
            if (simplified.mesh.indices.empty() || simplified.mesh.indices.size() >= input.indices.size()) {
                simplified.mesh = std::move(input);
                simplified.vertexError = 0.0f;
            }
            auto dataset = buildClusters(simplified.mesh);
            const auto firstCluster = static_cast<uint32_t>(next.dataset.clusters.size());
            appendMesh(next, simplified.mesh, dataset);
            float childError = 0.0f;
            for (uint32_t id : groups[groupId]) childError = std::max(childError, hierarchy.nodes[id].error);
            const float error = childError + simplified.vertexError;
            next.nodes.push_back(static_cast<uint32_t>(hierarchy.nodes.size() + parents.size()));
            parents.push_back({levelId, firstCluster, uint32_t(dataset.clusters.size()), groups[groupId],
                               group.boundsMin, group.boundsMax, error});
            next.maxError = std::max(next.maxError, error);
        }
        if (next.dataset.indices.size() >= current.dataset.indices.size()) break;
        next.mesh.indices = next.dataset.indices;
        hierarchy.nodes.insert(hierarchy.nodes.end(), parents.begin(), parents.end());
        hierarchy.levels.push_back(std::move(next));
    }
    groupNodes(hierarchy.levels.back(), hierarchy.nodes);
    hierarchy.roots = hierarchy.levels.back().nodes;
    return hierarchy;
}
