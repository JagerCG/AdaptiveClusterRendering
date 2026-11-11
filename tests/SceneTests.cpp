#include "SceneData.h"
#include "ClusterGpu.h"

#include <algorithm>
#include <fstream>
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
