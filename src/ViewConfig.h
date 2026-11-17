#pragma once
#include "Benchmark.h"
#include "RenderPlanning.h"
#include "SceneDiagnostics.h"
#include "HeightField.h"

struct ViewConfig {
    SceneSourceKind source = SceneSourceKind::Obj;
    std::filesystem::path sourcePath, heightPath;
    RenderSettings render;
    glm::vec3 pose{}; // yaw, pitch, distance / model radius
    int width = 960, height = 640;
    uint32_t level = 0, segments = 1;
    int selection = 0, boundsId = -1, heightPreset = 0;
    bool clusterColors = false, bounds = false, generated = false, wire = false, hidePanel = false;
    DisplacementSettings displacement;
    BenchmarkOptions sampling;
};
ViewConfig loadViewConfig(const std::filesystem::path& path);
void saveViewConfig(const std::filesystem::path& path, const ViewConfig& view);
LoadedScene loadViewScene(const ViewConfig& view);
void validateViewScene(const ViewConfig& view, const SceneData& scene);
