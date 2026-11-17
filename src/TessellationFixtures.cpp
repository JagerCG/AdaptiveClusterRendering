#include "TessellationFixtures.h"

#include <stdexcept>

namespace {
SceneData makePatchGrid(uint32_t cells)
{
    SceneData scene;
    scene.maxTrianglesPerCluster = 2;
    for (uint32_t y = 0; y <= cells; ++y) {
        for (uint32_t x = 0; x <= cells; ++x) {
            const glm::vec2 uv{float(x) / float(cells), float(y) / float(cells)};
            scene.mesh.positionIds.push_back(static_cast<uint32_t>(scene.mesh.vertices.size()));
            scene.mesh.vertices.push_back({{2 * uv.x - 1, 2 * uv.y - 1, 0}, {0, 0, 1}, uv});
        }
    }
    // Each cell is exactly one authored cluster. No clustering heuristic is run.
    for (uint32_t y = 0; y < cells; ++y) {
        for (uint32_t x = 0; x < cells; ++x) {
            const uint32_t a = y * (cells + 1) + x, b = a + 1;
            const uint32_t c = a + cells + 1, d = c + 1;
            const auto firstIndex = static_cast<uint32_t>(scene.mesh.indices.size());
            scene.mesh.indices.insert(scene.mesh.indices.end(), {a, b, d, a, d, c});
            scene.dataset.clusters.push_back({firstIndex, 6, scene.mesh.vertices[a].position,
                                                           scene.mesh.vertices[d].position});
        }
    }
    scene.dataset.indices = scene.mesh.indices;
    validateScene(scene);
    return scene;
}
} // namespace

SceneData makeTessellationFixture(TessellationFixture fixture)
{
    switch (fixture) {
    case TessellationFixture::Triangle: {
        SceneData scene;
        scene.maxTrianglesPerCluster = 1;
        scene.mesh.vertices = {
            {{-1, -1, 0}, {0, 0, 1}, {0, 0}},
            {{ 1, -1, 0}, {0, 0, 1}, {1, 0}},
            {{-1,  1, 0}, {0, 0, 1}, {0, 1}}
        };
        scene.mesh.indices = {0, 1, 2};
        scene.mesh.positionIds = {0, 1, 2};
        scene.dataset.indices = scene.mesh.indices;
        scene.dataset.clusters = {{0, 3, {-1, -1, 0}, {1, 1, 0}}};
        validateScene(scene);
        return scene;
    }
    case TessellationFixture::Plane: return makePatchGrid(1);
    case TessellationFixture::ClusterGrid: return makePatchGrid(2);
    }
    throw std::runtime_error("Unknown tessellation fixture.");
}
