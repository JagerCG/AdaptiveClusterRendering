#pragma once

#include "SceneData.h"
#include "TessellationFixtures.h"

#include <iosfwd>
#include <optional>

// Runtime diagnostics only. None of these fields are serialized in ACR v1.
enum class SceneSourceKind { Obj, Acr, Mock, Cube, Triangle, Plane, ClusterGrid, ShadowStage, Occlusion };

struct SceneSourceInfo {
    SceneSourceKind kind = SceneSourceKind::Mock;
    std::filesystem::path path; // absolute for file sources, empty for generated scenes
    std::optional<uint64_t> fileBytes;
    std::optional<uint32_t> fileVersion;
    double cpuPrepareMs = 0.0; // read/build + validation; excludes upload and framing
};

struct LoadedScene {
    SceneData scene;
    SceneSourceInfo source;
};

struct ScenePayloadBytes {
    uint64_t vertices = 0;
    uint64_t indices = 0; // reordered rendering indices, not original CPU indices
    uint64_t clusters = 0;
    uint64_t hierarchy = 0; // level, node and child records
    uint64_t total() const { return vertices + indices + clusters + hierarchy; }
};

struct SceneSaveResult {
    std::filesystem::path path;
    uint64_t fileBytes = 0;
    double cpuSaveMs = 0.0;
    double cpuReloadMs = 0.0; // verification read + validation, excludes comparison
};

LoadedScene loadSceneWithStats(const std::filesystem::path& path);
LoadedScene makeMockSceneWithStats();
LoadedScene makeCubeSceneWithStats();
LoadedScene makeFixtureSceneWithStats(TessellationFixture fixture);
const char* sceneSourceName(SceneSourceKind kind);
const char* scenePrepareLabel(SceneSourceKind kind);
ScenePayloadBytes scenePayloadBytes(const SceneData& scene);
void printSceneSummary(std::ostream& out, const LoadedScene& loaded);

// Exact comparison of persisted fields, including float bits, order and partition.
bool sameSceneData(const SceneData& a, const SceneData& b);
// Throws unless the saved file reloads into exactly the same source data.
SceneSaveResult saveSceneVerified(const std::filesystem::path& path, const SceneData& scene);
