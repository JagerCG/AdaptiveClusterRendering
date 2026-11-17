#include "SceneDiagnostics.h"
#include <glm/geometric.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
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
using Boundary = std::map<std::pair<uint32_t, uint32_t>, int>;
struct Surface {
    Boundary boundary;
    std::set<uint32_t> vertices;
    size_t triangles = 0;
    void triangle(uint32_t a, uint32_t b, uint32_t c)
    {
        ++triangles;
        const uint32_t v[]{a, b, c};
        for (int k = 0; k < 3; ++k) {
            auto x = v[k], y = v[(k + 1) % 3];
            vertices.insert(x);
            auto edge = std::minmax(x, y);
            if ((boundary[edge] += x < y ? 1 : -1) == 0) boundary.erase(edge);
        }
    }
};
void addNode(Surface& surface, const SceneData& scene, const LodNode& node)
{
    for (uint32_t i = 0; i < node.clusterCount; ++i) {
        const auto& c = sceneCluster(scene, node.firstCluster + i);
        const bool coarse = c.firstIndex >= scene.dataset.indices.size();
        const auto& indices = coarse ? scene.hierarchy.coarse.indices : scene.dataset.indices;
        const size_t base = coarse ? c.firstIndex - scene.dataset.indices.size() : c.firstIndex;
        for (size_t j = base; j < base + c.indexCount; j += 3)
            surface.triangle(indices[j], indices[j + 1], indices[j + 2]);
    }
}
void checkCuts(const SceneData& s)
{
    const auto& h = s.hierarchy;
    size_t reductions = 0;
    for (const auto& n : h.nodes) {
        if (!n.childCount) continue;
        Surface before, after;
        for (uint32_t i = 0; i < n.childCount; ++i)
            addNode(before, s, h.nodes[h.children[n.firstChild + i]]);
        addNode(after, s, n);
        check(before.boundary == after.boundary, "Parent changed oriented boundary edges or an attribute seam.");
        check(std::includes(before.vertices.begin(), before.vertices.end(), after.vertices.begin(),
                            after.vertices.end()),
              "Coarse geometry references a vertex outside its child regions.");
        for (int axis = 0; axis < 3; ++axis) {
            const auto extent = [&](const Surface& surface) {
                const auto ends =
                    std::minmax_element(surface.vertices.begin(), surface.vertices.end(), [&](auto a, auto b) {
                        return s.mesh.vertices[a].position[axis] < s.mesh.vertices[b].position[axis];
                    });
                return std::make_pair(s.mesh.vertices[*ends.first].position[axis],
                                      s.mesh.vertices[*ends.second].position[axis]);
            };
            check(extent(before) == extent(after), "Coarse geometry lost an extremity of the original region.");
        }
        check(after.triangles <= before.triangles, "Simplification added triangles.");
        if (after.triangles < before.triangles) {
            ++reductions;
            check(n.error > 0, "Missing geometric error.");
        }
        for (auto v : before.vertices) {
            if (after.vertices.count(v)) continue;
            bool covered = false;
            for (auto w : after.vertices)
                if (glm::distance(glm::dvec3(s.mesh.vertices[v].position), glm::dvec3(s.mesh.vertices[w].position)) <=
                    n.error) {
                    covered = true;
                    break;
                }
            check(covered, "Geometric error underestimates removed vertex travel.");
        }
    }
    check(reductions > 0, "Hierarchy has no simplified parent.");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        check(argc == 3, "Expected OBJ and scratch directory.");
        const std::filesystem::path dir = argv[2];
        std::filesystem::create_directories(dir);
        const auto s = loadScene(argv[1]);
        const auto& h = s.hierarchy;
        check(h.levels.size() >= 3 && h.levels.back().indexCount < s.dataset.indices.size() / 2,
              "Suzanne must contain real progressively coarser geometry.");
        checkCuts(s);
        auto repeated = buildScene(s.mesh, s.maxTrianglesPerCluster);
        check(repeated.hierarchy.levels.size() == h.levels.size(), "Unstable level count.");
        for (size_t i = 0; i < h.levels.size(); ++i)
            repeated.hierarchy.levels[i].buildMs = h.levels[i].buildMs;
        check(sameSceneData(s, repeated), "Building the same source changed geometry, error or hierarchy IDs.");
        const auto saved = saveSceneVerified(dir / "hierarchy.acr", s);
        auto restored = loadSceneWithStats(saved.path);
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
        const auto truncated = dir / "truncated.acr";
        std::filesystem::copy_file(saved.path, truncated, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::resize_file(truncated, saved.fileBytes - 1);
        reject([&] { (void)loadScene(truncated); });
        auto flat = s;
        flat.hierarchy = {};
        const auto v1 = saveSceneVerified(dir / "legacy.acr", flat);
        const auto legacy = loadSceneWithStats(v1.path);
        check(legacy.source.fileVersion == 1 && v1.fileBytes == 172576 && sameSceneData(flat, legacy.scene),
              "Legacy ACR v1 layout or compatibility changed.");
        MeshSource grid;
        constexpr uint32_t side = 17;
        for (uint32_t y = 0; y < side; ++y)
            for (uint32_t x = 0; x < side; ++x) {
                grid.vertices.push_back({{x / 16.f, y / 16.f, 0}, {0, 0, 1}, {x / 16.f, y / 16.f}});
                grid.positionIds.push_back(y * side + x);
                if (x && y) {
                    uint32_t d = y * side + x, a = d - side - 1;
                    grid.indices.insert(grid.indices.end(), {a, a + 1, d, a, d, d - 1});
                }
            }
        const auto open = buildScene(grid, 16);
        checkCuts(open);
        // Give the right half its own UVs and normals on the shared middle edge.
        auto split = grid;
        constexpr uint32_t middle = side / 2;
        const uint32_t splitStart = static_cast<uint32_t>(split.vertices.size());
        for (uint32_t y = 0; y < side; ++y) {
            const uint32_t original = y * side + middle;
            auto copy = split.vertices[original];
            copy.texcoord.x = 0;
            copy.normal = {0, 1, 0};
            split.vertices.push_back(copy);
            split.positionIds.push_back(original);
        }
        for (size_t i = 0; i < split.indices.size(); i += 3) {
            if (split.indices[i] % side < middle) continue;
            for (size_t corner = i; corner < i + 3; ++corner) {
                auto& v = split.indices[corner];
                if (v % side == middle) v = splitStart + v / side;
            }
        }
        checkCuts(buildScene(std::move(split), 16));
        const auto faceted = dir / "no-normals.obj";
        std::ofstream obj(faceted);
        for (const auto& v : grid.vertices)
            obj << "v " << v.position.x << ' ' << v.position.y << " 0\n";
        for (size_t i = 0; i < grid.indices.size(); i += 3)
            obj << "f " << grid.indices[i] + 1 << ' ' << grid.indices[i + 1] + 1 << ' ' << grid.indices[i + 2] + 1
                << '\n';
        obj.close();
        const auto unwelded = loadScene(faceted);
        check(unwelded.hierarchy.levels.back().indexCount < grid.indices.size() / 2,
              "Identical flat-shaded OBJ corners were mistaken for hard seams.");
        std::cout << "Verified all " << h.levels.size()
                  << " Suzanne levels, mixed-level boundaries, error, "
                     "open-grid perimeter, UV/normal seams, deterministic links, ACR v1/v2 and invalid inputs.\n";
    }
    catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
