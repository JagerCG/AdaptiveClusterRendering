#pragma once

#include "ClusterBuilder.h"

enum class ClusterSelectionMode { All, Explicit };

// Runtime source identity. Zero means no uploaded scene. Never serialized in ACR.
using SceneRevision = uint64_t;

struct ClusterSelection {
    SceneRevision sceneRevision = 0;
    ClusterSelectionMode mode = ClusterSelectionMode::All;
    std::vector<uint32_t> clusterIds; // All requires empty; Explicit empty means no geometry
};

struct SelectedClusterRange {
    uint32_t sourceClusterId;
    uint32_t firstIndex; // source index elements, not bytes
    uint32_t indexCount;
};

struct ResolvedClusterSelection {
    SceneRevision sceneRevision = 0;
    bool allClusters = false;
    std::vector<SelectedClusterRange> ranges; // request order; All is ascending ID
    uint64_t indexCount = 0;
    uint64_t triangleCount() const { return indexCount / 3; }
};

// Source metadata must come from a validated SceneData. Selection changes do not
// rebuild clusters, edit source geometry, or compact/renumber source IDs.
ResolvedClusterSelection resolveClusterSelection(const std::vector<Cluster>& clusters,
    uint32_t sourceIndexCount, SceneRevision currentRevision, const ClusterSelection& selection);
