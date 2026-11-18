#include "OrbitCamera.h"
#include "TessellationFixtures.h"
#include "VulkanRenderer.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint32_t> ids(const ResolvedClusterSelection& selection)
{
    std::vector<uint32_t> result;
    for (const auto& range : selection.ranges) result.push_back(range.sourceClusterId);
    return result;
}
} // namespace

int runShadowCasterChecks(VulkanRenderer& renderer, GLFWwindow* window, const SceneData& realScene, OrbitCamera realCamera)
{
    auto stage = makeTessellationFixture(TessellationFixture::ShadowCasters);
    renderer.uploadScene(stage);
    OrbitCamera camera({0,0,0}, 2);
    camera.setPose({0,0,1.5f});
    RenderSettings settings;
    settings.shadows = true;
    settings.shadowCoverage = .30f;
    settings.frustumPreview = true;
    const auto draw = [&](bool generated = false) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            glfwPollEvents();
            if (!renderer.prepareFrame(window)) continue;
            renderer.setRenderSettings(settings);
            if (generated) renderer.ensureGeneratedGeometry();
            if (renderer.drawFrame(camera, false, false, -1, generated, false, nullptr, true)) {
                renderer.finishRenderMeasurement();
                return;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("Timed out rendering the shadow acceptance scene.");
    };
    const auto select = [&](std::vector<uint32_t> chosen) {
        renderer.setClusterSelection({renderer.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, std::move(chosen)});
    };
    // Only change the caster switch between renders. Compare the complete depth images,
    // including empty pixels, under exactly the same projection, selection and displacement.
    const auto compare = [&](bool generated) {
        settings.shadowCulling = false;
        draw(generated);
        const auto unculled = renderer.readShadowDepth();
        const auto main = renderer.readCameraDepth();
        const auto light = renderer.lastLightMatrix();
        settings.shadowCulling = true;
        draw(generated);
        expect(light == renderer.lastLightMatrix() && unculled == renderer.readShadowDepth(),
               "Light-frustum culling changed a shadow-map pixel or the light matrix.");
        expect(main == renderer.readCameraDepth(), "Shadow culling altered main-view geometry.");
    };

    compare(false);
    const std::vector<uint32_t> mainIds{0,2,3}, casterIds{0,1};
    expect(ids(renderer.clusterSelection()) == std::vector<uint32_t>({0,1,2,3}) &&
           renderer.lastRecordedDraw().visibleClusters == mainIds.size() && ids(renderer.shadowSelection()) == casterIds,
           "The camera preview and light did not draw the authored clusters.");
    RenderSettings cpu;
    cpu.frustumPreview = true;
    std::vector<uint32_t> expected;
    for (uint32_t id = 0; id < stage.dataset.clusters.size(); ++id)
        if (clusterVisible(stage.dataset.clusters[id], 0, renderer.lastLightMatrix(), {}, cpu)) expected.push_back(id);
    expect(expected == casterIds && renderer.shadowVisibilityStats().frustum == 2,
           "Light GPU results disagree with the CPU six-plane reference.");

    // Both centers are on the same light ray. Inspect the actual texel sampled at
    // the on-screen receiver and remove only its off-camera caster as a negative control.
    const auto receiver = renderer.lastLightMatrix() * glm::vec4(0,0,-.7f,1);
    const auto caster = renderer.lastLightMatrix() * glm::vec4(1.08f,1.44f,1.1f,1);
    expect(glm::length(glm::vec2(receiver-caster)) < 1e-4f && std::abs(receiver.x) < 1 && std::abs(receiver.y) < 1,
           "The receiver and caster should project onto the same interior light texel.");
    const uint32_t size = VulkanRenderer::shadowMapSize;
    const auto px = uint32_t((receiver.x * .5f + .5f) * size);
    const auto py = uint32_t((receiver.y * .5f + .5f) * size);
    const auto sample = size_t(py) * size + px;
    const auto withCaster = renderer.readShadowDepth();
    expect(std::abs(withCaster[sample] - caster.z) < .003f && withCaster[sample] < receiver.z - settings.shadowBias,
           "The off-camera caster did not shadow the visible receiver.");
    select(mainIds);
    draw();
    expect(std::abs(renderer.readShadowDepth()[sample] - receiver.z) < .003f,
           "Removing the off-camera caster did not uncover the receiver.");

    renderer.setEdgeSegments(2);
    renderer.setDisplacement({true,.08f});
    select({2,1,0});
    compare(true);
    expect(ids(renderer.clusterSelection()) == std::vector<uint32_t>({2,1,0}) &&
           ids(renderer.shadowSelection()) == std::vector<uint32_t>({1,0}) &&
           renderer.lastRecordedDraw().shadowTriangles == 16,
           "Shadow generation did not preserve candidate order and displacement.");
    select({2,3});
    compare(true);
    const auto empty = renderer.readShadowDepth();
    expect(renderer.lastRecordedDraw().triangles == 16 && !renderer.lastRecordedDraw().shadowTriangles &&
           std::all_of(empty.begin(), empty.end(), [](float d){ return d == 1; }),
           "An empty caster list must clear the map and retain camera geometry.");
    select({1});
    compare(true);
    expect(!renderer.lastRecordedDraw().triangles && renderer.lastRecordedDraw().shadowTriangles == 8,
           "A fully camera-culled list lost its off-camera shadow caster.");
    select({});
    compare(false);
    expect(!renderer.lastRecordedDraw().shadowCandidates && !renderer.lastRecordedDraw().shadowCasters,
           "Empty authored input fell back to all casters.");

    renderer.setClusterSelection({renderer.sceneGpuData().sceneRevision, ClusterSelectionMode::All, {}});
    compare(true);
    const auto fixedLight = renderer.lastLightMatrix();
    const auto fixedMap = renderer.readShadowDepth();
    const auto fixedIds = ids(renderer.shadowSelection());
    camera.setPose({3.14159265f,0,1.5f});
    draw(true);
    expect(renderer.lastLightMatrix() == fixedLight &&
           ids(renderer.shadowSelection()) == fixedIds && renderer.readShadowDepth() == fixedMap,
           "Moving the camera changed a fixed light view.");
    settings.lightDirection = {-1,.7f,1};
    compare(true);
    expect(renderer.lastLightMatrix() != fixedLight && renderer.readShadowDepth() != fixedMap,
           "Changing light direction left a stale matrix or shadow image.");
    settings.shadowCoverage = 1;
    compare(false);
    expect(renderer.lastRecordedDraw().shadowCasters == 4, "Full scene fit should retain all fixture casters.");
    settings.shadows = false;
    settings.display = RenderDisplay::ShadowMap;
    draw();
    expect(renderer.lastRecordedDraw().shadowPass && !renderer.lastRecordedDraw().triangles &&
           renderer.lastRecordedDraw().shadowTriangles == 8, "Light-only display lost its caster pass.");
    settings.display = RenderDisplay::Shaded;
    draw();
    expect(!renderer.lastRecordedDraw().shadowPass && !renderer.lastRecordedDraw().shadowCasters &&
           !renderer.shadowVisibilityStats().gpuCullMs, "Inactive shadow pass retained caster statistics.");

    // Move a side marker toward the center using its authored normal and a white height map.
    for (auto& v : stage.mesh.vertices) v.normal = {-1,0,0};
    renderer.uploadScene(stage);
    select({3});
    settings.shadows = true;
    settings.lightDirection = {0,0,1};
    settings.shadowCoverage = .30f;
    settings.frustumPreview = false;
    draw();
    expect(renderer.shadowSelection().ranges.empty(), "Undisplaced side marker should be outside the light.");
    renderer.setHeightField(makeHeightField(HeightPreset::White));
    renderer.setDisplacement({true,1.45f});
    compare(true);
    const auto moved = renderer.readShadowDepth();
    expect(ids(renderer.shadowSelection()) == std::vector<uint32_t>({3}) &&
           *std::min_element(moved.begin(), moved.end()) < 1,
           "Displacement moved geometry into the light, but its padded caster bounds were rejected.");

    renderer.uploadScene(realScene);
    camera = realCamera;
    settings = {};
    settings.shadows = true;
    settings.shadowCoverage = .45f;
    renderer.setHeightField(makeHeightField(HeightPreset::Waves));
    renderer.setDisplacement({true,.08f});
    compare(false);
    compare(true);
    renderer.verifyGeneratedGeometry(realScene);
    const auto counts = renderer.lastRecordedDraw();
    expect(counts.shadowCasters > 0 && counts.shadowCasters < counts.shadowCandidates &&
           counts.shadowTriangles == 4 * renderer.shadowSelection().triangleCount(),
           "Displaced fixed input and light-space caster selection disagree.");
    std::cout << "Verified off-camera shadows, depth images, empty and reversed lists, displacement and real mesh casters.\n";
    return 0;
}
