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
    case TessellationFixture::Occlusion: {
        // Four patches: foreground wall, fully hidden patch, partly exposed patch, off-screen patch.
        const auto tile = makePatchGrid(2);
        SceneData scene;
        scene.maxTrianglesPerCluster = tile.maxTrianglesPerCluster;
        const auto patch = [&](glm::vec3 center, float scale) {
            const uint32_t vertexBase = static_cast<uint32_t>(scene.mesh.vertices.size());
            const uint32_t indexBase = static_cast<uint32_t>(scene.dataset.indices.size());
            for (auto v : tile.mesh.vertices) {
                v.position = v.position * scale + center;
                scene.mesh.positionIds.push_back(static_cast<uint32_t>(scene.mesh.vertices.size()));
                scene.mesh.vertices.push_back(v);
            }
            for (auto i : tile.dataset.indices)
                scene.dataset.indices.push_back(vertexBase + i);
            for (auto c : tile.dataset.clusters) {
                c.firstIndex += indexBase;
                c.boundsMin = c.boundsMin * scale + center;
                c.boundsMax = c.boundsMax * scale + center;
                scene.dataset.clusters.push_back(c);
            }
        };
        patch({-.25f, 0, .45f}, 1.05f);
        patch({-.25f, 0, -.75f}, .38f);
        patch({1.12f, 0, -.5f}, .55f);
        patch({8, 0, 0}, .35f);
        scene.mesh.indices = scene.dataset.indices;
        buildMeshHierarchy(scene, 0);
        validateScene(scene);
        return scene;
    }
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
    case TessellationFixture::ShadowStage: {
        SceneData scene;
        scene.maxTrianglesPerCluster = 2;
        const auto quad = [&](float halfSize, float z, bool receiver) {
            const uint32_t base = static_cast<uint32_t>(scene.mesh.vertices.size());
            const glm::vec2 corners[] = {{0,0}, {1,0}, {1,1}, {0,1}};
            for (const auto& uv : corners) {
                scene.mesh.positionIds.push_back(static_cast<uint32_t>(scene.mesh.vertices.size()));
                // The receiver samples neutral at UV (0,0) for Waves/Mound.
                scene.mesh.vertices.push_back({{(2*uv.x-1)*halfSize, (2*uv.y-1)*halfSize, z}, {0,0,1}, receiver ? glm::vec2(0) : uv});
            }
            const auto first = static_cast<uint32_t>(scene.mesh.indices.size());
            scene.mesh.indices.insert(scene.mesh.indices.end(), {base,base+1,base+2, base,base+2,base+3});
            scene.dataset.clusters.push_back({first,6,{-halfSize,-halfSize,z},{halfSize,halfSize,z}});
        };
        quad(1.8f, -.65f, true);
        quad(.6f, .3f, false);
        scene.dataset.indices = scene.mesh.indices;
        validateScene(scene);
        return scene;
    }
    case TessellationFixture::Plane: return makePatchGrid(1);
    case TessellationFixture::ClusterGrid: return makePatchGrid(2);
    }
    throw std::runtime_error("Unknown tessellation fixture.");
}
