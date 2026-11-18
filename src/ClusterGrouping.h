#pragma once

#include <cstdint>

struct MeshSource;
struct ClusterDataset;

void buildClusterGroups(const MeshSource& mesh, ClusterDataset& dataset, uint32_t maxClustersPerGroup = 8);
