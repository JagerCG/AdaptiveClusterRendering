#pragma once

#include "ClusterBuilder.h"

#include <cstddef>

struct QemClusterErrorStats {
    double meanInternalEdgeError = 0.0;
    size_t internalAdjacencyCount = 0;
};

struct QemClusterBuildResult {
    ClusterDataset dataset;
    QemClusterErrorStats errorStats;
};

// Experimental QEM-guided grouping. It changes triangle grouping, not geometry.
QemClusterBuildResult buildQemGuidedClusters(const MeshSource& mesh, uint32_t maxTrianglesPerCluster = 64);
QemClusterErrorStats measureQemInternalEdgeError(const MeshSource& mesh, const ClusterDataset& dataset);
