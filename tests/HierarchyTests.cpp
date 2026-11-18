#include "SceneDiagnostics.h"
#include "TessellationFixtures.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
template <class F> void reject(F f)
{
    try {
        f();
    }
    catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("Malformed hierarchy was accepted.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 2, "Expected a scratch directory.");
        const std::filesystem::path dir = argv[1];
        std::filesystem::create_directories(dir);
        auto s = makeTessellationFixture(TessellationFixture::Plane);
        const auto fine = s.dataset.clusters.front();
        auto& h = s.hierarchy;
        // An authored two-level cache exercises the file and GPU contracts independently of any builder.
        h.coarse.indices.assign(s.dataset.indices.begin(), s.dataset.indices.begin() + 3);
        h.coarse.clusters = {{6, 3, fine.boundsMin, fine.boundsMax}};
        h.levels = {{0, 1, 0, 1, 0, 6, 0}, {1, 1, 1, 1, 6, 3, 0}};
        h.nodes = {{fine.boundsMin, 0, fine.boundsMax, 1, 0, 1, 0, 0},
                   {fine.boundsMin, 1, fine.boundsMax, kNoParent, 1, 1, 0, 1}};
        h.children = {0};
        const auto saved = saveSceneVerified(dir / "hierarchy.acr", s);
        const auto restored = loadSceneWithStats(saved.path);
        check(restored.source.fileVersion == 2 && sameSceneData(s, restored.scene), "ACR v2 roundtrip failed.");
        auto damaged = s;
        damaged.hierarchy.children[0] = UINT32_MAX;
        reject([&] { validateScene(damaged); });
        reject([&] { (void)saveSceneVerified(saved.path, damaged); });
        check(sameSceneData(loadScene(saved.path), s), "A failed save replaced the previously valid file.");
        damaged = s;
        damaged.hierarchy.nodes.back().error = -1;
        reject([&] { validateScene(damaged); });
        damaged = s;
        damaged.hierarchy.coarse.indices.front() = UINT32_MAX;
        reject([&] { validateScene(damaged); });
        damaged = s;
        damaged.hierarchy.levels[1].firstCluster++;
        reject([&] { validateScene(damaged); });
        damaged = s;
        damaged.hierarchy.levels.front().maxDiameterPixels = 5;
        reject([&] { validateScene(damaged); });
        const auto truncated = dir / "truncated.acr";
        std::filesystem::copy_file(saved.path, truncated, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::resize_file(truncated, saved.fileBytes - 1);
        reject([&] { (void)loadScene(truncated); });
        auto flat = s;
        flat.hierarchy = {};
        const auto v1 = saveSceneVerified(dir / "legacy.acr", flat);
        const auto legacy = loadSceneWithStats(v1.path);
        check(legacy.source.fileVersion == 1 && sameSceneData(flat, legacy.scene), "ACR v1 roundtrip failed.");
        std::cout << "Verified authored ACR v2 levels, v1 compatibility and invalid input handling.\n";
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
