#include "ClusterGpu.h"
#include "ClusterSelection.h"
#include "SceneDiagnostics.h"
#include "TessellationFixtures.h"

#include <glm/geometric.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>

namespace {
constexpr SceneRevision kSelectionRevision = 7;
void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void rejected(const std::function<void()>& action)
{
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, "Invalid fixture/selection request was accepted.");
}
bool near(float a, float b) { return std::abs(a - b) < 1e-6f; }

struct FixtureSpec {
    TessellationFixture fixture;
    SceneSourceKind sourceKind;
    uint32_t vertices, triangles, clusters, sharedEdges, boundaryEdges, crossClusterEdges;
    float area;
    uint64_t fileBytes, gpuBytes;
};

void checkFixture(const FixtureSpec& spec, const std::filesystem::path& directory)
{
    const auto loaded = makeFixtureSceneWithStats(spec.fixture);
    const auto& scene = loaded.scene;
    validateScene(scene);
    expect(scene.mesh.vertices.size() == spec.vertices && scene.dataset.indices.size() == spec.triangles * 3 &&
           scene.dataset.clusters.size() == spec.clusters, "Unexpected fixture element counts.");
    expect(loaded.source.kind == spec.sourceKind && loaded.source.path.empty() &&
           !loaded.source.fileBytes && !loaded.source.fileVersion, "Fixture must be a generated source.");
    expect(std::isfinite(loaded.source.cpuPrepareMs) && loaded.source.cpuPrepareMs >= 0,
           "Invalid fixture preparation time.");
    expect(sameSceneData(scene, makeTessellationFixture(spec.fixture)), "Fixture construction is not deterministic.");

    for (uint32_t i = 0; i < scene.mesh.vertices.size(); ++i) {
        const auto& vertex = scene.mesh.vertices[i];
        expect(vertex.position.z == 0 && vertex.normal == glm::vec3(0, 0, 1), "Expected XY plane with +Z normals.");
        expect(near(glm::length(vertex.normal), 1), "Fixture normal is not normalized.");
        expect(vertex.texcoord.x >= 0 && vertex.texcoord.x <= 1 && vertex.texcoord.y >= 0 && vertex.texcoord.y <= 1,
               "UV is outside [0,1].");
        expect(near(vertex.position.x, 2 * vertex.texcoord.x - 1) &&
               near(vertex.position.y, 2 * vertex.texcoord.y - 1), "UV orientation / position mapping changed.");
        expect(scene.mesh.positionIds[i] == i, "Fixture topology ID changed.");
    }

    struct EdgeUse { uint32_t from, to, cluster; };
    std::map<std::pair<uint32_t, uint32_t>, std::vector<EdgeUse>> edges;
    float area = 0;
    for (uint32_t id = 0; id < scene.dataset.clusters.size(); ++id) {
        const auto& cluster = scene.dataset.clusters[id];
        const uint32_t trianglesPerCluster = spec.fixture == TessellationFixture::Triangle ? 1u : 2u;
        expect(cluster.firstIndex == id * trianglesPerCluster * 3 && cluster.indexCount == trianglesPerCluster * 3,
               "Authored cluster ranges changed.");
        for (uint32_t first = cluster.firstIndex; first < cluster.firstIndex + cluster.indexCount; first += 3) {
            const auto a = scene.dataset.indices[first], b = scene.dataset.indices[first + 1], c = scene.dataset.indices[first + 2];
            const glm::vec3 cross = glm::cross(scene.mesh.vertices[b].position - scene.mesh.vertices[a].position,
                                             scene.mesh.vertices[c].position - scene.mesh.vertices[a].position);
            expect(cross.z > 0 && cross.x == 0 && cross.y == 0, "Degenerate or reversed fixture triangle.");
            area += .5f * cross.z;
            for (auto edge : {std::make_pair(a, b), std::make_pair(b, c), std::make_pair(c, a)}) {
                edges[std::minmax(edge.first, edge.second)].push_back({edge.first, edge.second, id});
            }
        }
        if (spec.fixture == TessellationFixture::ClusterGrid) {
            const glm::vec3 minimum{float(id % 2) - 1, float(id / 2) - 1, 0};
            expect(cluster.boundsMin == minimum && cluster.boundsMax == minimum + glm::vec3(1, 1, 0),
                   "Fixed cluster IDs must run left-to-right, bottom-to-top.");
        }
    }
    expect(near(area, spec.area), "Fixture area changed (possible missing / duplicated surface).");
    uint32_t shared = 0, boundary = 0, acrossClusters = 0;
    for (const auto& entry : edges) {
        const auto& uses = entry.second;
        expect(uses.size() == 1 || uses.size() == 2, "Non-manifold fixture edge.");
        if (uses.size() == 1) { ++boundary; continue; }
        ++shared;
        expect(uses[0].from == uses[1].to && uses[0].to == uses[1].from,
               "Neighbors must share identical vertex/UV/normal endpoints in opposite winding.");
        if (uses[0].cluster != uses[1].cluster) ++acrossClusters;
    }
    expect(shared == spec.sharedEdges && boundary == spec.boundaryEdges && acrossClusters == spec.crossClusterEdges,
           "Fixture seam / adjacency structure changed.");
    expect(scenePayloadBytes(scene).total() == spec.gpuBytes, "Fixture GPU payload size changed.");
    const auto path = directory / "fixture.acr";
    const auto saved = saveSceneVerified(path, scene);
    const auto restored = loadSceneWithStats(path);
    expect(saved.fileBytes == spec.fileBytes && sameSceneData(scene, restored.scene),
           "Fixture ACR roundtrip changed source fields / storage size.");
    const auto packed = packClusters(restored.scene.dataset);
    for (uint32_t id = 0; id < packed.size(); ++id) {
        expect(packed[id].indexRange.z == id && packed[id].indexRange.x == scene.dataset.clusters[id].firstIndex,
               "Serialized fixture lost stable cluster IDs / ranges.");
    }
    std::cout << "Fixture reference: " << spec.vertices << " vertices, " << spec.triangles << " triangles, "
              << spec.clusters << " clusters; shared edges=" << shared << ", cross-cluster edges=" << acrossClusters << '\n';
}

void checkSelections()
{
    const auto scene = makeTessellationFixture(TessellationFixture::ClusterGrid);
    const auto original = scene;
    constexpr SceneRevision revision = kSelectionRevision;
    const auto resolve = [&](const ClusterSelection& selection, SceneRevision current = kSelectionRevision) {
        return resolveClusterSelection(scene.dataset.clusters, static_cast<uint32_t>(scene.dataset.indices.size()), current, selection);
    };
    const auto all = resolve({revision, ClusterSelectionMode::All, {}});
    expect(all.allClusters && all.ranges.size() == 4 && all.indexCount == 24 && all.triangleCount() == 8,
           "All must include every source cluster.");
    for (uint32_t id = 0; id < 4; ++id) {
        expect(all.ranges[id].sourceClusterId == id && all.ranges[id].firstIndex == id * 6 &&
               all.ranges[id].indexCount == 6, "All must use ascending original IDs and ranges.");
    }
    const auto partial = resolve({revision, ClusterSelectionMode::Explicit, {3, 0}});
    expect(!partial.allClusters && partial.ranges.size() == 2 && partial.triangleCount() == 4 &&
           partial.ranges[0].sourceClusterId == 3 && partial.ranges[0].firstIndex == 18 &&
           partial.ranges[1].sourceClusterId == 0 && partial.ranges[1].firstIndex == 0,
           "Explicit selection must retain request order and original IDs.");
    const auto empty = resolve({revision, ClusterSelectionMode::Explicit, {}});
    expect(!empty.allClusters && empty.ranges.empty() && empty.indexCount == 0, "Empty selection became All.");
    const auto one = resolve({revision, ClusterSelectionMode::Explicit, {2}});
    expect(one.ranges.size() == 1 && one.triangleCount() == 2 && one.ranges[0].sourceClusterId == 2,
           "Single-cluster selection changed.");
    const auto reordered = resolve({revision, ClusterSelectionMode::Explicit, {3, 2, 1, 0}});
    expect(!reordered.allClusters && reordered.triangleCount() == 8 && reordered.ranges.front().sourceClusterId == 3,
           "An explicit full list was silently reordered as All.");
    rejected([&] { (void)resolve({revision, ClusterSelectionMode::Explicit, {1, 1}}); });
    rejected([&] { (void)resolve({revision, ClusterSelectionMode::Explicit, {4}}); });
    rejected([&] { (void)resolve({revision, ClusterSelectionMode::Explicit, {std::numeric_limits<uint32_t>::max()}}); });
    rejected([&] { (void)resolve({revision, ClusterSelectionMode::All, {0}}); });
    rejected([&] { (void)resolve({revision, static_cast<ClusterSelectionMode>(99), {}}); });
    rejected([&] { (void)resolve({revision - 1, ClusterSelectionMode::All, {}}); });
    rejected([&] { (void)resolve({revision, ClusterSelectionMode::Explicit, {}}, revision + 1); });
    rejected([&] { (void)resolve({0, ClusterSelectionMode::All, {}}, 0); });
    expect(sameSceneData(scene, original), "Selection resolution changed source geometry / partition.");
    rejected([&] { (void)makeTessellationFixture(static_cast<TessellationFixture>(99)); });
}
} // namespace

int main(int argc, char** argv)
{
    try {
        expect(argc == 3, "Expected: fixture case, scratch directory.");
        const std::string test = argv[1];
        const std::filesystem::path directory = argv[2];
        std::filesystem::create_directories(directory);
        if (test == "triangle") checkFixture({TessellationFixture::Triangle, SceneSourceKind::Triangle,
            3, 1, 1, 0, 3, 0, 2, 244, 156}, directory);
        else if (test == "plane") checkFixture({TessellationFixture::Plane, SceneSourceKind::Plane,
            4, 2, 1, 1, 4, 0, 4, 304, 200}, directory);
        else if (test == "grid") checkFixture({TessellationFixture::ClusterGrid, SceneSourceKind::ClusterGrid,
            9, 8, 4, 8, 8, 4, 4, 772, 576}, directory);
        else if (test == "selection") checkSelections();
        else throw std::runtime_error("Unknown fixture test.");
        std::cout << "PASS: " << test << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
