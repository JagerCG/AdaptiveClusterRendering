#pragma once

#include "ClusterBuilder.h"
#include "MeshSource.h"

#include <filesystem>

// Track A -> B -> C handoff. Original indices and position IDs are retained for
// later hierarchy construction; rendering uses dataset.indices exclusively.
struct SceneData {
    MeshSource mesh;
    ClusterDataset dataset;
    uint32_t maxTrianglesPerCluster = 64;
};

constexpr uint32_t kSceneFileVersion = 1;

SceneData buildScene(MeshSource mesh, uint32_t maxTrianglesPerCluster = 64);
SceneData makeMockScene();
void validateScene(const SceneData& scene);
// .obj builds clusters; .acr reads the preprocessed data without rebuilding it.
SceneData loadScene(const std::filesystem::path& path);
void saveScene(const std::filesystem::path& path, const SceneData& scene);
