#include "VulkanRenderer.h"
#include "OrbitCamera.h"
#include "TessellationFixtures.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void checkPlaneDepth(const std::vector<float>& depth, const glm::mat4& light, float z)
{
    const auto inverse = glm::inverse(light);
    const auto size = VulkanRenderer::shadowMapSize;
    // Sample pixel centers well inside a known world-space plane, independently
    // intersect their orthographic rays with it and compare the stored GPU depth.
    for (float x : {-.6f, 0.f, .6f}) for (float y : {-.6f, 0.f, .6f}) {
        auto clip = light * glm::vec4(x, y, z, 1); clip /= clip.w;
        const auto px = uint32_t((clip.x * .5f + .5f) * size), py = uint32_t((clip.y * .5f + .5f) * size);
        expect(px < size && py < size, "Light fit clipped the displaced plane.");
        const glm::vec2 ndc{2 * (float(px) + .5f) / size - 1, 2 * (float(py) + .5f) / size - 1};
        auto nearPoint = inverse * glm::vec4(ndc, 0, 1), farPoint = inverse * glm::vec4(ndc, 1, 1);
        nearPoint /= nearPoint.w; farPoint /= farPoint.w;
        const float reference = (z - nearPoint.z) / (farPoint.z - nearPoint.z);
        expect(std::abs(depth[size_t(py) * size + px] - reference) < 2e-5f,
               "Shadow depth does not match the selected source/displaced geometry and light transform.");
    }
    for (float d : depth) expect(std::isfinite(d) && d >= 0 && d <= 1, "Invalid shadow depth.");
}
}

int runRenderIntegrationChecks(VulkanRenderer& renderer, GLFWwindow* window)
{
    OrbitCamera camera({0, 0, 0}, 2);
    RenderSettings settings; settings.lightDirection = {0, 0, 1};
    renderer.uploadScene(makeTessellationFixture(TessellationFixture::Plane));
    renderer.setHeightField(makeHeightField(HeightPreset::White));
    renderer.setEdgeSegments(4); renderer.setDisplacement({true, .35f});
    uint32_t frames = 0;
    const auto render = [&](bool generated, bool colors = false, bool wire = false) {
        renderer.setRenderSettings(settings);
        if (generated) renderer.ensureGeneratedGeometry();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            glfwPollEvents();
            expect(std::chrono::steady_clock::now() < deadline, "Integration render timed out.");
            if (renderer.prepareFrame(window) && renderer.drawFrame(camera, colors, true, -1, generated, wire, nullptr, true)) break;
        }
        const auto total = renderer.finishRenderMeasurement();
        if (renderer.supportsTimestamps()) {
            expect(total && renderer.lastMainMs() && *total >= *renderer.lastMainMs(), "Missing/inconsistent main timestamps.");
            expect(renderer.lastShadowMs().has_value() == renderer.lastRecordedDraw().shadowPass, "Wrong shadow timing availability.");
        }
        ++frames;
    };
    render(false); // default shadows OFF must bind a valid initialized descriptor
    settings.shadows = true; render(false);
    checkPlaneDepth(renderer.readShadowDepth(), renderer.lastLightMatrix(), 0);
    render(true);
    const auto displaced = renderer.readShadowDepth();
    checkPlaneDepth(displaced, renderer.lastLightMatrix(), .35f);
    expect(renderer.lastRecordedDraw().shadowTriangles == 32 && renderer.lastRecordedDraw().triangles == 32,
           "Shadow/main paths did not consume the n=4 generated geometry.");
    const auto generation = renderer.generatedGeometry().generationRevision;
    const auto light = renderer.lastLightMatrix();
    camera.orbit(190, -35); render(true);
    expect(std::memcmp(&light, &renderer.lastLightMatrix(), sizeof(light)) == 0 && renderer.readShadowDepth() == displaced,
           "Camera movement changed the directional shadow projection.");
    settings.shadowBias = .02f; render(true);
    expect(renderer.readShadowDepth() == displaced, "Receiver bias unexpectedly changed caster depth.");
    settings.lightDirection = {.6f, .8f, 1}; render(true);
    checkPlaneDepth(renderer.readShadowDepth(), renderer.lastLightMatrix(), .35f);
    for (auto mode : {RenderDisplay::Depth, RenderDisplay::ShadowMap, RenderDisplay::Shaded}) {
        settings.display = mode; render(true, true, renderer.supportsWireframe());
        expect(renderer.lastRecordedDraw().shadowTriangles == 32, "Display change lost displaced shadow casters.");
        if (mode == RenderDisplay::ShadowMap)
            expect(renderer.lastRecordedDraw().meshDrawCalls == 0 && renderer.lastRecordedDraw().boundsInstances == 0,
                   "Shadow map display should show light depth, not camera meshes/bounds.");
    }
    expect(renderer.generatedGeometry().generationRevision == generation, "Camera/light/display regenerated unchanged geometry.");
    settings.frustumPreview = true; settings.maxDistance = .001f;
    for (bool colors : {false, true}) {
        render(true, colors);
        expect(renderer.lastRecordedDraw().triangles == 0 && renderer.lastRecordedDraw().boundsInstances == 0 &&
               renderer.lastRecordedDraw().shadowTriangles == 32, "Empty CPU visibility fell back to All or removed shadow casters.");
    }
    settings.frustumPreview = false; settings.maxDistance = 0;
    renderer.setClusterSelection({renderer.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, {}});
    for (bool generated : {false, true}) for (bool colors : {false, true}) {
        render(generated, colors);
        const auto depth = renderer.readShadowDepth();
        expect(renderer.lastRecordedDraw().triangles == 0 && renderer.lastRecordedDraw().shadowTriangles == 0 &&
               renderer.lastRecordedDraw().boundsInstances == 0, "Explicit empty selection drew geometry.");
        expect(std::all_of(depth.begin(), depth.end(), [](float d) { return d == 1; }), "Empty selection retained stale shadows.");
    }
    settings.display = RenderDisplay::ShadowMap; render(true); settings.display = RenderDisplay::Shaded;
    const auto previousRevision = renderer.sceneGpuData().sceneRevision;
    renderer.uploadScene(makeTessellationFixture(TessellationFixture::ClusterGrid));
    expect(renderer.sceneGpuData().sceneRevision != previousRevision, "Source identity was reused.");
    renderer.setClusterSelection({renderer.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, {3, 0}});
    renderer.setEdgeSegments(2);
    for (bool generated : {false, true}) for (bool colors : {false, true}) {
        render(generated, colors);
        const auto triangles = generated ? 16u : 4u;
        expect(renderer.lastRecordedDraw().triangles == triangles && renderer.lastRecordedDraw().shadowTriangles == triangles,
               "Subset rendering depends on color mode or uses source counts for generated geometry.");
        expect(renderer.clusterSelection().ranges[0].sourceClusterId == 3 &&
               renderer.clusterSelection().ranges[1].sourceClusterId == 0, "Original cluster IDs/order changed.");
    }
    glfwSetWindowSize(window, 700, 510); render(true);
    renderer.setDisplacement({false, .35f}); render(true);
    renderer.setClusterSelection({renderer.sceneGpuData().sceneRevision, ClusterSelectionMode::All, {}});
    render(false); const auto original = renderer.readShadowDepth();
    render(true); const auto neutral = renderer.readShadowDepth();
    size_t changed = 0;
    for (size_t i = 0; i < original.size(); ++i) if (std::abs(original[i] - neutral[i]) > 2e-5f) ++changed;
    expect(changed < 128, "Neutral subdivision changed shadow geometry beyond raster edge tolerance.");
    settings.shadows = false; render(true);
    expect(renderer.lastRecordedDraw().shadowTriangles == 0, "Disabling shadows still submitted caster geometry.");
    std::cout << "Render integration passed: " << frames << " frames; displaced depth reference, light/camera cache, "
                 "source/generated subsets, empty/culled draws, wireframe/depth/light view, resize and pass timestamps.\n";
    return 0;
}
