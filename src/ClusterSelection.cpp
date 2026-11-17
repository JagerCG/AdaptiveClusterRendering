#include "ClusterSelection.h"

#include <stdexcept>

ResolvedClusterSelection resolveClusterSelection(const std::vector<Cluster>& clusters,
    uint32_t sourceIndexCount, SceneRevision currentRevision, const ClusterSelection& selection)
{
    if (currentRevision == 0 || selection.sceneRevision != currentRevision) {
        throw std::runtime_error("Cluster selection belongs to a different source scene revision.");
    }
    if (selection.mode != ClusterSelectionMode::All && selection.mode != ClusterSelectionMode::Explicit) {
        throw std::runtime_error("Unknown cluster selection mode.");
    }
    if (selection.mode == ClusterSelectionMode::All && !selection.clusterIds.empty()) {
        throw std::runtime_error("All selection must not also specify cluster IDs.");
    }
    ResolvedClusterSelection result;
    result.sceneRevision = currentRevision;
    result.allClusters = selection.mode == ClusterSelectionMode::All;
    result.ranges.reserve(result.allClusters ? clusters.size() : selection.clusterIds.size());
    std::vector<bool> seen(clusters.size(), false);
    const auto append = [&](uint32_t id) {
        if (id >= clusters.size()) throw std::runtime_error("Selected cluster ID is out of range.");
        if (seen[id]) throw std::runtime_error("Selected cluster IDs must be unique.");
        seen[id] = true;
        const auto& cluster = clusters[id];
        if (cluster.indexCount == 0 || cluster.indexCount % 3 != 0 ||
            uint64_t(cluster.firstIndex) + cluster.indexCount > sourceIndexCount) {
            throw std::runtime_error("Selected cluster has an invalid source index range.");
        }
        result.ranges.push_back({id, cluster.firstIndex, cluster.indexCount});
        result.indexCount += cluster.indexCount;
    };
    if (result.allClusters) {
        for (size_t id = 0; id < clusters.size(); ++id) append(static_cast<uint32_t>(id));
    } else {
        for (uint32_t id : selection.clusterIds) append(id);
    }
    return result;
}
