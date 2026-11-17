#include "SceneDiagnostics.h"

#include "ClusterGpu.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <string>

namespace {
using Clock = std::chrono::steady_clock;
double elapsedMs(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
bool sameVec3(glm::vec3 a, glm::vec3 b)
{
    return std::memcmp(&a.x, &b.x, 3 * sizeof(float)) == 0;
}
std::string lowerExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

// Reserve a private sibling directory atomically, even across processes.
// Renaming within the same filesystem publishes only a fully verified file.
class PendingSave {
  public:
    explicit PendingSave(const std::filesystem::path& target)
    {
        static std::atomic<uint64_t> serial{0};
        do {
            directory =
                target.parent_path() / (".acr-save-" + std::to_string(Clock::now().time_since_epoch().count()) + "-" +
                                        std::to_string(serial.fetch_add(1)));
            file = directory / "scene.acr";
        } while (!std::filesystem::create_directory(directory));
    }
    ~PendingSave()
    {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::filesystem::remove(directory, ignored);
    }
    std::filesystem::path file;

  private:
    std::filesystem::path directory;
};
} // namespace

LoadedScene loadSceneWithStats(const std::filesystem::path& path)
{
    const auto start = Clock::now();
    LoadedScene result;
    result.source.path = std::filesystem::absolute(path).lexically_normal();
    result.scene = loadScene(result.source.path); // rejects unsupported formats / versions
    result.source.kind = lowerExtension(path) == ".acr" ? SceneSourceKind::Acr : SceneSourceKind::Obj;
    if (result.source.kind == SceneSourceKind::Acr) result.source.fileVersion = sceneFileVersion(result.scene);
    result.source.fileBytes = std::filesystem::file_size(result.source.path);
    result.source.cpuPrepareMs = elapsedMs(start);
    return result;
}

LoadedScene makeMockSceneWithStats()
{
    const auto start = Clock::now();
    LoadedScene result;
    result.scene = makeMockScene();
    result.source.kind = SceneSourceKind::Mock;
    result.source.cpuPrepareMs = elapsedMs(start);
    return result;
}

LoadedScene makeCubeSceneWithStats()
{
    const auto start = Clock::now();
    LoadedScene result;
    result.scene = buildScene(makeCube());
    result.source.kind = SceneSourceKind::Cube;
    result.source.cpuPrepareMs = elapsedMs(start);
    return result;
}

LoadedScene makeFixtureSceneWithStats(TessellationFixture fixture)
{
    const auto start = Clock::now();
    LoadedScene result;
    result.scene = makeTessellationFixture(fixture);
    switch (fixture) {
    case TessellationFixture::Occlusion: result.source.kind = SceneSourceKind::Occlusion; break;
    case TessellationFixture::ShadowStage: result.source.kind = SceneSourceKind::ShadowStage; break;
    case TessellationFixture::Triangle: result.source.kind = SceneSourceKind::Triangle; break;
    case TessellationFixture::Plane: result.source.kind = SceneSourceKind::Plane; break;
    case TessellationFixture::ClusterGrid: result.source.kind = SceneSourceKind::ClusterGrid; break;
    }
    result.source.cpuPrepareMs = elapsedMs(start);
    return result;
}

const char* sceneSourceName(SceneSourceKind kind)
{
    switch (kind) {
    case SceneSourceKind::Obj: return "OBJ";
    case SceneSourceKind::Occlusion: return "Occlusion stage";
    case SceneSourceKind::Acr: return "ACR";
    case SceneSourceKind::Mock: return "Mock (4 clusters)";
    case SceneSourceKind::Cube: return "Cube";
    case SceneSourceKind::ShadowStage: return "Shadow stage (receiver + patch)";
    case SceneSourceKind::Triangle: return "Test triangle";
    case SceneSourceKind::Plane: return "UV plane";
    case SceneSourceKind::ClusterGrid: return "Cluster grid (2 x 2)";
    }
    return "Unknown";
}

const char* scenePrepareLabel(SceneSourceKind kind)
{
    switch (kind) {
    case SceneSourceKind::Obj: return "CPU read/build/validate";
    case SceneSourceKind::Acr: return "CPU read/validate";
    default: return "CPU build/validate";
    }
}

ScenePayloadBytes scenePayloadBytes(const SceneData& scene)
{
    return {uint64_t(scene.mesh.vertices.size()) * sizeof(Vertex),
            uint64_t(scene.dataset.indices.size() + scene.hierarchy.coarse.indices.size()) * sizeof(uint32_t),
            uint64_t(scene.dataset.clusters.size() + scene.hierarchy.coarse.clusters.size()) * sizeof(GpuClusterDesc),
            scene.hierarchy.levels.size() * sizeof(LodLevel) + scene.hierarchy.nodes.size() * sizeof(LodNode) +
                scene.hierarchy.children.size() * sizeof(uint32_t)};
}

void printSceneSummary(std::ostream& out, const LoadedScene& loaded)
{
    const auto& source = loaded.source;
    const auto& scene = loaded.scene;
    out << "Source: " << sceneSourceName(source.kind);
    if (source.fileVersion) out << " v" << *source.fileVersion;
    if (!source.path.empty()) out << " | " << source.path.u8string();
    out << "\nFile bytes: ";
    if (source.fileBytes)
        out << *source.fileBytes;
    else
        out << "N/A (generated scene)";
    out << "\nVertices: " << scene.mesh.vertices.size() << " | triangles: " << scene.dataset.indices.size() / 3
        << " | clusters: " << scene.dataset.clusters.size() << " | capacity: " << scene.maxTrianglesPerCluster << '\n'
        << scenePrepareLabel(source.kind) << " (ms): " << source.cpuPrepareMs << '\n';
    const auto bytes = scenePayloadBytes(scene);
    out << "GPU payload bytes (vertices / indices / clusters / hierarchy / total): " << bytes.vertices << " / "
        << bytes.indices << " / " << bytes.clusters << " / " << bytes.hierarchy << " / " << bytes.total() << '\n';
    for (size_t i = 0; i < scene.hierarchy.levels.size(); ++i) {
        const auto& l = scene.hierarchy.levels[i];
        float error = 0;
        for (uint32_t j = 0; j < l.nodeCount; ++j) error = std::max(error,scene.hierarchy.nodes[l.firstNode+j].error);
        out << "LOD " << i << " | " << l.indexCount/3 << " triangles | " << l.clusterCount << " clusters | "
            << l.nodeCount << " nodes | max error " << error << " | build " << l.buildMs << " ms\n";
    }
}

bool sameSceneData(const SceneData& a, const SceneData& b)
{
    const auto sameRecords = [](const auto& x, const auto& y) {
        return x.size() == y.size() && (x.empty() || std::memcmp(x.data(),y.data(),x.size()*sizeof(x[0])) == 0);
    };
    if (!sameRecords(a.hierarchy.levels,b.hierarchy.levels) || !sameRecords(a.hierarchy.nodes,b.hierarchy.nodes) ||
        a.hierarchy.children != b.hierarchy.children || a.hierarchy.coarse.indices != b.hierarchy.coarse.indices ||
        a.hierarchy.coarse.clusters.size() != b.hierarchy.coarse.clusters.size()) return false;
    if (a.maxTrianglesPerCluster != b.maxTrianglesPerCluster ||
        a.mesh.indices != b.mesh.indices || a.mesh.positionIds != b.mesh.positionIds ||
        a.dataset.indices != b.dataset.indices || a.mesh.vertices.size() != b.mesh.vertices.size() ||
        a.dataset.clusters.size() != b.dataset.clusters.size()) return false;
    for (size_t i = 0; i < a.mesh.vertices.size(); ++i) {
        const auto& x = a.mesh.vertices[i];
        const auto& y = b.mesh.vertices[i];
        if (!sameVec3(x.position, y.position) || !sameVec3(x.normal, y.normal) ||
            std::memcmp(&x.texcoord.x, &y.texcoord.x, 2 * sizeof(float)) != 0) return false;
    }
    for (uint32_t i = 0; i < a.dataset.clusters.size() + a.hierarchy.coarse.clusters.size(); ++i) {
        const auto& x = sceneCluster(a,i);
        const auto& y = sceneCluster(b,i);
        if (x.firstIndex != y.firstIndex || x.indexCount != y.indexCount ||
            !sameVec3(x.boundsMin, y.boundsMin) || !sameVec3(x.boundsMax, y.boundsMax)) return false;
    }
    return true;
}

SceneSaveResult saveSceneVerified(const std::filesystem::path& path, const SceneData& scene)
{
    // Check before opening/truncating: verification loads by extension, and an OBJ
    // source must never be overwritten with a binary cache under the wrong name.
    if (lowerExtension(path) != ".acr") throw std::runtime_error("Save the scene with an .acr extension.");
    SceneSaveResult result;
    result.path = std::filesystem::absolute(path).lexically_normal();
    const auto start = Clock::now();
    PendingSave pending(result.path);
    saveScene(pending.file, scene);
    result.cpuSaveMs = elapsedMs(start);
    const auto restored = loadSceneWithStats(pending.file);
    if (!sameSceneData(scene, restored.scene)) {
        throw std::runtime_error("Saved ACR does not match the source geometry, clusters or hierarchy.");
    }
    std::filesystem::rename(pending.file,result.path);
    result.fileBytes = *restored.source.fileBytes;
    result.cpuReloadMs = restored.source.cpuPrepareMs;
    return result;
}
