#include "SceneData.h"
#include "SceneDiagnostics.h"
#include "ClusterGpu.h"
#include "QemClusterBuilder.h"
#include "ObjLoader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
void rejected(const std::function<void()>& action)
{
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    expect(failed, "Invalid scene was accepted.");
}
std::vector<char> readBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    expect(bool(in), "Cannot open test file.");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void writeBytes(const std::filesystem::path& path, const std::vector<char>& bytes)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    expect(bool(out), "Cannot write test file.");
}
void roundtrip(const SceneData& scene, const std::filesystem::path& dir)
{
    const auto a = dir / "first.acr", b = dir / "second.acr";
    saveScene(a, scene);
    const auto restored = loadScene(a);
    saveScene(b, restored);
    expect(readBytes(a) == readBytes(b), "Roundtrip changed vertex/UV/normal/index/position ID/cluster data.");
    expect(scene.dataset.indices == restored.dataset.indices, "Roundtrip changed cluster order.");
    expect(scene.mesh.indices == restored.mesh.indices, "Roundtrip changed source order.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        expect(argc == 4, "Expected: case, real OBJ, scratch directory.");
        const std::string test = argv[1];
        const std::filesystem::path dir = argv[3];
        std::filesystem::create_directories(dir);
        const auto mock = makeMockScene();
        if (test == "mock") {
            expect(mock.dataset.clusters.size() == 4 && mock.mesh.vertices.size() == 16 &&
                   mock.dataset.indices.size() == 48, "Mock must contain four fixed tetrahedra.");
            const auto gpu = packClusters(mock.dataset);
            for (uint32_t id = 0; id < 4; ++id) {
                expect(gpu[id].indexRange == glm::uvec4(id * 12, 12, id, 0), "Incorrect GPU range/ID.");
                expect(glm::vec3(gpu[id].boundsMin) == mock.dataset.clusters[id].boundsMin &&
                       gpu[id].boundsMin.w == 0 && gpu[id].boundsMax.w == 0, "Incorrect GPU bounds/padding.");
            }
            // Mock is authored separately, so validate the regular builder against it.
            const auto rebuilt = buildScene(mock.mesh, 4);
            expect(rebuilt.dataset.clusters.size() == 4, "Disconnected tetrahedra were merged.");
        } else if (test == "roundtrip") {
            roundtrip(mock, dir);
        } else if (test == "real-model") {
            const auto scene = loadScene(argv[2]);
            expect(scene.mesh.vertices.size() == 2012 && scene.dataset.indices.size() == 3936 * 3,
                   "Suzanne source geometry changed.");
            roundtrip(scene, dir);
            const auto again = buildScene(scene.mesh);
            expect(again.dataset.indices == scene.dataset.indices &&
                   again.dataset.clusters.size() == scene.dataset.clusters.size(), "Clustering is not deterministic.");
            validateScene(buildScene(scene.mesh, 128));
        } else if (test == "qem-guided") {
            const auto mesh = loadObj(argv[2]);
            constexpr uint32_t capacity = 64;
            const auto baselineStart = std::chrono::steady_clock::now();
            const auto baseline = buildClusters(mesh, capacity);
            const auto baselineTime = std::chrono::steady_clock::now() - baselineStart;
            const auto qemStart = std::chrono::steady_clock::now();
            const auto qem = buildQemGuidedClusters(mesh, capacity);
            const auto qemTime = std::chrono::steady_clock::now() - qemStart;
            const auto repeated = buildQemGuidedClusters(mesh, capacity);
            expect(qem.dataset.indices == repeated.dataset.indices &&
                   qem.dataset.clusters.size() == repeated.dataset.clusters.size(),
                   "QEM-guided clustering is not deterministic.");
            // PR #3 retains baseline membership and only sorts triangles within each cluster.
            expect(qem.dataset.clusters.size() == baseline.clusters.size(), "QEM ordering changed baseline membership.");
            for (size_t id = 0; id < baseline.clusters.size(); ++id) {
                const auto& a = baseline.clusters[id]; const auto& b = qem.dataset.clusters[id];
                expect(a.firstIndex == b.firstIndex && a.indexCount == b.indexCount &&
                       a.boundsMin == b.boundsMin && a.boundsMax == b.boundsMax, "QEM ordering changed cluster bounds/ranges.");
            }
            const size_t triangleCount = mesh.indices.size() / 3;
            const size_t minimumClusters = (triangleCount + capacity - 1) / capacity;
            expect(qem.dataset.clusters.size() >= minimumClusters &&
                   qem.dataset.clusters.size() <= triangleCount,
                   "QEM-guided grouping violated cluster count bounds.");
            const SceneData qemScene{mesh, qem.dataset, capacity};
            validateScene(qemScene);
            roundtrip(qemScene, dir);
            const auto baselineError = measureQemInternalEdgeError(mesh, baseline);
            expect(std::isfinite(qem.errorStats.meanInternalEdgeError) &&
                   qem.errorStats.meanInternalEdgeError >= 0.0 &&
                   std::isfinite(baselineError.meanInternalEdgeError) &&
                   baselineError.meanInternalEdgeError >= 0.0,
                   "Expected finite, nonnegative QEM error statistics.");
            expect(qem.errorStats.internalAdjacencyCount > 0 && baselineError.internalAdjacencyCount > 0,
                   "Expected Suzanne clusters to contain adjacent triangle pairs.");
            expect(std::abs(qem.errorStats.meanInternalEdgeError -
                       measureQemInternalEdgeError(mesh, qem.dataset).meanInternalEdgeError) <=
                       1e-12 * std::max(1.0, std::abs(qem.errorStats.meanInternalEdgeError)),
                   "QEM grouping reported inconsistent internal-edge error.");
            const auto baselineMs = std::chrono::duration<double, std::milli>(baselineTime).count();
            const auto qemMs = std::chrono::duration<double, std::milli>(qemTime).count();
            std::cout << std::fixed << std::setprecision(3)
                      << "baseline clusters=" << baseline.clusters.size() << " build_ms=" << baselineMs
                      << " mean_internal_QEM_error=" << baselineError.meanInternalEdgeError << '\n'
                      << "QEM clusters=" << qem.dataset.clusters.size() << " build_ms=" << qemMs
                      << " mean_internal_QEM_error=" << qem.errorStats.meanInternalEdgeError << '\n'
                      << "Geometry is unchanged; geometric deviation is zero by construction.\n";
        } else if (test == "storage-stats") {
            const auto generated = makeMockSceneWithStats();
            expect(generated.source.kind == SceneSourceKind::Mock && generated.source.path.empty() &&
                   !generated.source.fileBytes && !generated.source.fileVersion,
                   "Generated mock must not report a disk file / format version.");
            const auto payload = scenePayloadBytes(generated.scene);
            expect(payload.vertices == 512 && payload.indices == 192 && payload.clusters == 192 &&
                   payload.total() == 896, "Incorrect mock GPU payload bytes.");
            const auto path = dir / "verified.ACR";
            const auto saved = saveSceneVerified(path, generated.scene);
            expect(saved.fileBytes == 1216 && saved.fileBytes == std::filesystem::file_size(path),
                   "ACR v1 file bytes must include CPU indices, position IDs and the header.");
            auto restored = loadSceneWithStats(path);
            expect(restored.source.kind == SceneSourceKind::Acr && restored.source.fileVersion == kSceneFileVersion &&
                   restored.source.fileBytes == saved.fileBytes && restored.source.path.is_absolute() &&
                   sameSceneData(restored.scene, generated.scene), "ACR summary / exact roundtrip mismatch.");
            const auto obj = loadSceneWithStats(argv[2]);
            expect(obj.source.kind == SceneSourceKind::Obj && !obj.source.fileVersion &&
                   obj.source.fileBytes == std::filesystem::file_size(argv[2]), "OBJ source metadata mismatch.");
            const auto realSaved = saveSceneVerified(dir / "suzanne.acr", obj.scene);
            expect(realSaved.fileBytes == 172576 && scenePayloadBytes(obj.scene).total() == 117232,
                   "Unexpected Suzanne storage / GPU payload size.");
            const auto cube = makeCubeSceneWithStats();
            expect(cube.source.kind == SceneSourceKind::Cube && !cube.source.fileBytes &&
                   !cube.source.fileVersion && cube.source.path.empty(), "Cube must be a generated source.");
            for (double ms : {generated.source.cpuPrepareMs, restored.source.cpuPrepareMs,
                              obj.source.cpuPrepareMs, cube.source.cpuPrepareMs, saved.cpuSaveMs, saved.cpuReloadMs}) {
                expect(std::isfinite(ms) && ms >= 0.0, "Invalid CPU timing (no speed threshold is required).");
            }
            // Detect changes even when counts remain identical. Signed zero is persisted bit-for-bit.
            auto changed = mock; changed.mesh.vertices[0].normal.x = -0.0f;
            expect(!sameSceneData(mock, changed), "Float bit changes were ignored.");
            changed = mock; changed.mesh.vertices[0].position.x += .1f;
            expect(!sameSceneData(mock, changed), "Vertex changes were ignored.");
            changed = mock; changed.mesh.vertices[0].texcoord.x = .5f;
            expect(!sameSceneData(mock, changed), "UV changes were ignored.");
            changed = mock; std::swap(changed.mesh.indices[0], changed.mesh.indices[1]);
            expect(!sameSceneData(mock, changed), "Original index order changes were ignored.");
            changed = mock; std::swap(changed.dataset.indices[0], changed.dataset.indices[1]);
            expect(!sameSceneData(mock, changed), "Rendering index order changes were ignored.");
            changed = mock; changed.mesh.positionIds[0] = 1;
            expect(!sameSceneData(mock, changed), "Position ID changes were ignored.");
            changed = mock; changed.dataset.clusters[0].boundsMin.x -= .1f;
            expect(!sameSceneData(mock, changed), "Cluster bound changes were ignored.");
            changed = mock; changed.dataset.clusters[0].indexCount = 3;
            expect(!sameSceneData(mock, changed), "Cluster range changes were ignored.");
            changed = mock; changed.maxTrianglesPerCluster = 8;
            expect(!sameSceneData(mock, changed), "Capacity changes were ignored.");
            const auto oldSource = restored.source;
            auto bytes = readBytes(path); bytes[0] = 'X';
            writeBytes(dir / "invalid.acr", bytes);
            rejected([&] { restored = loadSceneWithStats(dir / "invalid.acr"); });
            expect(sameSceneData(restored.scene, mock) && restored.source.path == oldSource.path &&
                   restored.source.fileBytes == oldSource.fileBytes &&
                   restored.source.cpuPrepareMs == oldSource.cpuPrepareMs,
                   "Failed load overwrote previously valid scene / metadata.");
            rejected([&] { (void)saveSceneVerified(dir / "missing" / "scene.acr", mock); });
            const auto wrongExtension = dir / "keep.obj";
            const std::vector<char> keep{'K', 'E', 'E', 'P'};
            writeBytes(wrongExtension, keep);
            rejected([&] { (void)saveSceneVerified(wrongExtension, mock); });
            expect(readBytes(wrongExtension) == keep, "Wrong-extension output was overwritten before rejection.");
        } else if (test == "invalid-scene") {
            auto damaged = mock;
            damaged.dataset.indices[0] = 99999;
            rejected([&] { validateScene(damaged); });
            damaged = mock; damaged.dataset.clusters[1].firstIndex = 0;
            rejected([&] { validateScene(damaged); });
            damaged = mock; damaged.dataset.clusters[0].boundsMin.x = 100;
            rejected([&] { validateScene(damaged); });
            damaged = mock; damaged.mesh.vertices[0].normal.x = std::numeric_limits<float>::quiet_NaN();
            rejected([&] { validateScene(damaged); });
            damaged = mock; damaged.maxTrianglesPerCluster = 3;
            rejected([&] { validateScene(damaged); });
            damaged = mock; std::swap(damaged.dataset.indices[0], damaged.dataset.indices[1]);
            rejected([&] { validateScene(damaged); });
            damaged = mock; damaged.mesh.positionIds.pop_back();
            rejected([&] { validateScene(damaged); });
        } else if (test == "corrupt-file") {
            const auto good = dir / "good.acr", bad = dir / "bad.acr";
            saveScene(good, mock);
            const auto original = readBytes(good);
            const auto checkBytes = [&](const std::vector<char>& bytes) {
                writeBytes(bad, bytes);
                rejected([&] { (void)loadScene(bad); });
            };
            auto bytes = original; bytes[0] = 'X'; checkBytes(bytes);
            bytes = original; bytes[8] = 2; checkBytes(bytes); // version
            bytes = original; bytes[36] = 16; checkBytes(bytes); // vertex stride
            bytes = original; bytes[16] = char(0xff); checkBytes(bytes); // count/length mismatch
            bytes = original; bytes.pop_back(); checkBytes(bytes);
            bytes = original; bytes.push_back(0); checkBytes(bytes);
            bytes = original; bytes[bytes.size() - 8] = 99; checkBytes(bytes); // stable cluster ID
            bytes = original; bytes.resize(12); checkBytes(bytes);
            // A structurally valid payload with a non-finite position must also fail.
            bytes = original; bytes[64] = 0; bytes[65] = 0; bytes[66] = char(0x80); bytes[67] = char(0x7f);
            checkBytes(bytes);
            // Rejection must not affect a subsequent valid load.
            (void)loadScene(good);
        } else {
            throw std::runtime_error("Unknown test.");
        }
        std::cout << "PASS: " << test << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
