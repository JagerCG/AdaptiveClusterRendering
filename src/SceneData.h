#pragma once

#include "ClusterBuilder.h"
#include "MeshSource.h"
#include "MeshHierarchy.h"

#include <filesystem>

// Source indices and position IDs preserve OBJ topology. Rendering shares the
// vertex array across the original partition and all precomputed coarse levels.
struct SceneData {
    MeshSource mesh;
    ClusterDataset dataset;
    uint32_t maxTrianglesPerCluster = 64;
    MeshHierarchy hierarchy;
};

constexpr uint32_t kSceneFileVersion = 2;
inline uint32_t sceneFileVersion(const SceneData& scene) { return scene.hierarchy.levels.empty() ? 1u : kSceneFileVersion; }

SceneData buildScene(MeshSource mesh, uint32_t maxTrianglesPerCluster = 64);
SceneData makeMockScene();
void validateScene(const SceneData& scene);
// OBJ imports the M1 flat partition. ACR restores any geometry levels supplied in the file.
SceneData loadScene(const std::filesystem::path& path);
void saveScene(const std::filesystem::path& path, const SceneData& scene);
