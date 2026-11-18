#include "VulkanRenderer.h"
#include "OrbitCamera.h"
#include "TessellationFixtures.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void expect(bool condition, const char* error)
{
    if (!condition) throw std::runtime_error(error);
}

// Independent top-down traversal. Project all eight AABB corners instead of the shader's support point.
std::vector<uint32_t> referenceCut(const SceneData& s, const OrbitCamera& camera, VkExtent2D size, float pixels,
                                   float pad)
{
    const auto& h = s.hierarchy;
    const auto aspect = float(size.width) / size.height;
    const glm::dmat4 vp = camera.viewProjection(aspect);
    auto lo = s.dataset.clusters.front().boundsMin, hi = s.dataset.clusters.front().boundsMax;
    for (const auto& cluster : s.dataset.clusters) {
        lo = glm::min(lo, cluster.boundsMin);
        hi = glm::max(hi, cluster.boundsMax);
    }
    const float footprint = projectedDiameterPixels(lo - pad, hi + pad, vp, size.width, size.height);
    const auto excluded = std::find_if(h.levels.begin(), h.levels.end(), [&](const LodLevel& level) {
        return level.maxDiameterPixels && footprint > level.maxDiameterPixels;
    });
    const auto eligible = excluded == h.levels.end() ? h.nodes.size() : excluded->firstNode;
    std::vector<uint32_t> todo, ids;
    for (uint32_t n = 0; n < h.levels.back().nodeCount; ++n)
        todo.push_back(h.levels.back().firstNode + n);
    while (!todo.empty()) {
        const auto id = todo.back();
        const auto& node = h.nodes[id];
        todo.pop_back();
        double nearest = std::numeric_limits<double>::infinity();
        for (int corner = 0; corner < 8; ++corner) {
            const glm::dvec3 point{corner & 1 ? node.boundsMax.x + pad : node.boundsMin.x - pad,
                                   corner & 2 ? node.boundsMax.y + pad : node.boundsMin.y - pad,
                                   corner & 4 ? node.boundsMax.z + pad : node.boundsMin.z - pad};
            nearest = std::min(nearest, (vp * glm::dvec4(point, 1)).w);
        }
        if (!node.childCount || (id < eligible && pixels > 0 && nearest > camera.nearPlane(aspect) &&
                                 node.error * double(camera.focalPixels(size.height)) / nearest <= pixels)) {
            for (uint32_t i = 0; i < node.clusterCount; ++i)
                ids.push_back(node.firstCluster + i);
        }
        else {
            for (uint32_t c = 0; c < node.childCount; ++c)
                todo.push_back(h.children[node.firstChild + c]);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void planeDepth(const CameraDepthView& info, const std::vector<float>& pixels, double z)
{
    const auto w = info.extent.width, h = info.extent.height;
    expect(info.view && info.sampler && info.solid && info.frame &&
               info.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL && pixels.size() == size_t(w) * h,
           "Incomplete camera depth handoff.");
    const glm::dmat4 vp = info.viewProjection, inverse = glm::inverse(vp);
    for (double x : {-.3, 0., .3})
        for (double y : {-.3, 0., .3}) {
            auto clip = vp * glm::dvec4(x, y, z, 1);
            clip /= clip.w;
            const uint32_t px = uint32_t((clip.x * .5 + .5) * w), py = uint32_t((clip.y * .5 + .5) * h);
            expect(px < w && py < h, "Plane sample outside viewport.");
            glm::dvec4 ray{2 * (px + .5) / w - 1, 2 * (py + .5) / h - 1, 0, 1};
            auto a = inverse * ray;
            ray.z = 1;
            auto b = inverse * ray;
            a /= a.w;
            b /= b.w;
            const auto hit = a + (b - a) * ((z - a.z) / (b.z - a.z));
            const auto projected = vp * hit;
            expect(std::abs(pixels[size_t(py) * w + px] - projected.z / projected.w) < 3e-5,
                   "Camera depth disagrees with projected plane.");
        }
    expect(std::all_of(pixels.begin(), pixels.end(), [](float d) { return std::isfinite(d) && d >= 0 && d <= 1; }),
           "Invalid camera depth pixel.");
}

void checkSuzanneOutline(VkExtent2D extent, const std::vector<float>& fine, const std::vector<float>& coarse)
{
    const auto firstCoveredRow = [&](const std::vector<float>& pixels, uint32_t x) {
        for (uint32_t y = 0; y < extent.height; ++y)
            if (pixels[size_t(y) * extent.width + x] < 1.f) return int(y);
        return int(extent.height);
    };
    // At the authored front pose these columns span the top of the skull, between the ears.
    int maxDeviation = 0, maxAsymmetry = 0;
    for (uint32_t x = 5 * extent.width / 12; x < extent.width / 2; ++x) {
        const uint32_t mirror = extent.width - 1 - x;
        const int left = firstCoveredRow(coarse, x), right = firstCoveredRow(coarse, mirror);
        const int fineLeft = firstCoveredRow(fine, x), fineRight = firstCoveredRow(fine, mirror);
        expect(fineLeft < int(extent.height) && fineRight < int(extent.height), "Reference crown outside the viewport.");
        maxDeviation = std::max({maxDeviation, std::abs(left - fineLeft), std::abs(right - fineRight)});
        maxAsymmetry = std::max(maxAsymmetry, std::abs(left - right));
    }
    std::cout << "Frontal crown contour deviation / mirror difference: " << maxDeviation << " / " << maxAsymmetry << " px.\n";
    // Count each mirrored pixel pair once. The union covers the projected half-silhouette.
    size_t coveredPairs = 0, asymmetricPairs = 0;
    for (uint32_t y = 0; y < extent.height; ++y) {
        const auto row = size_t(y) * extent.width;
        for (uint32_t x = 0; x < extent.width / 2; ++x) {
            const bool left = coarse[row + x] < 1.f;
            const bool right = coarse[row + extent.width - 1 - x] < 1.f;
            coveredPairs += left || right;
            asymmetricPairs += left != right;
        }
    }
    std::cout << "Full silhouette asymmetric pairs: " << asymmetricPairs << " of " << coveredPairs << ".\n";
    expect(maxDeviation <= 2 && maxAsymmetry <= 2, "Suzanne's adaptive LOD visibly distorted the frontal crown.");
    expect(coveredPairs && 100 * asymmetricPairs <= coveredPairs, "Suzanne's frontal silhouette lost symmetry.");
}

void checkSuzanneCrownHeight(const CameraDepthView& info, const std::vector<float>& fine, const std::vector<float>& coarse)
{
    const glm::dmat4 inverse = glm::inverse(glm::dmat4(info.viewProjection));
    const auto w = info.extent.width, h = info.extent.height;
    size_t samples = 0;
    double maxChange = 0;
    for (uint32_t y = 0; y < h; y += 4) {
        for (uint32_t x = 0; x < w; x += 4) {
            const size_t i = size_t(y) * w + x;
            if (fine[i] >= 1.f) continue;
            const auto world = [&](float depth) {
                const auto p = inverse * glm::dvec4(2. * (x + .5) / w - 1., 2. * (y + .5) / h - 1., depth, 1.);
                return glm::dvec3(p) / p.w;
            };
            const auto reference = world(fine[i]);
            // Sample the interior of the original crown. The ears and grazing silhouette
            // have real depth discontinuities when the covered pixel changes.
            if (reference.y <= .70 || std::abs(reference.x) >= .55 || reference.z <= -.65 || reference.z >= .40)
                continue;
            expect(coarse[i] < 1.f, "The coarse crown contains a hole.");
            maxChange = std::max(maxChange, std::abs(reference.y - world(coarse[i]).y));
            ++samples;
        }
    }
    std::cout << "Overhead crown height change: " << maxChange << " model units across " << samples << " samples.\n";
    expect(samples > 100 && maxChange < .035, "Suzanne's adaptive LOD lost too much crown height.");
}

bool silhouetteWithin(VkExtent2D size, const std::vector<float>& from, const std::vector<float>& to, int pixels)
{
    const int w = static_cast<int>(size.width), h = static_cast<int>(size.height);
    bool covered = false;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const auto i = size_t(y) * w + x;
            if (from[i] >= 1) continue;
            covered = true;
            if (to[i] < 1) continue;
            bool found = false;
            for (int row = std::max(0, y - pixels); !found && row <= std::min(h - 1, y + pixels); ++row)
                for (int col = std::max(0, x - pixels); col <= std::min(w - 1, x + pixels); ++col)
                    found |= to[size_t(row) * w + col] < 1;
            if (!found) return false;
        }
    }
    return covered;
}
} // namespace

int runLodDepthChecks(VulkanRenderer& r, GLFWwindow* window, const SceneData& scene, OrbitCamera camera, bool suzanneShape)
{
    expect(scene.hierarchy.levels.size() >= 3, "Expected a real multi-level scene.");
    RenderSettings s;
    s.levelColors = true;
    const auto prep = [&] {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!r.prepareFrame(window)) {
            glfwPollEvents();
            expect(std::chrono::steady_clock::now() < deadline, "Viewport did not become available.");
        }
    };
    const auto draw = [&](bool generated = false, bool wire = false) {
        r.setRenderSettings(s);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            glfwPollEvents();
            prep();
            if (s.adaptiveLod) r.selectLod(camera, generated ? r.plannedDisplacementBound() : 0);
            if (generated) r.ensureGeneratedGeometry();
            if (r.drawFrame(camera, false, !suzanneShape, -1, generated, wire, nullptr)) break;
            expect(std::chrono::steady_clock::now() < deadline, "LOD/depth draw timed out.");
        }
    };
    if (suzanneShape) {
        camera.reset();
        auto wheel = camera;
        for (int step = 0; step < 60; ++step) wheel.zoom(-1.f);
        expect(std::abs(wheel.pose().z - 256.f) < .001f, "Scroll wheel could not reach the extreme-distance limit.");
        auto restored = camera;
        restored.setPose(wheel.pose());
        wheel.zoom(1.f);
        expect(wheel.pose().z < restored.pose().z && wheel.pose().z > 12.f, "Scroll wheel could not return from the limit.");
        const float distance = camera.pose().z;
        const auto capture = [&](float pitch, float budget) {
            camera.setPose({0, pitch, distance});
            s.adaptiveLod = true;
            s.lodPixels = budget;
            draw();
            return r.readCameraDepth();
        };
        const auto fineFront = capture(0, 0);
        const uint64_t fineTriangles = r.lastRecordedDraw().triangles;
        const auto coarseFront = capture(0, 96);
        const uint64_t coarseTriangles = r.lastRecordedDraw().triangles;
        expect(4 * coarseTriangles <= 3 * fineTriangles,
               "Shape check requires at least a quarter fewer triangles than the source.");
        checkSuzanneOutline(r.renderExtent(), fineFront, coarseFront);
        const auto fineTop = capture(1.463f, 0);
        const auto coarseTop = capture(1.463f, 96);
        checkSuzanneCrownHeight(r.cameraDepth(), fineTop, coarseTop);
        const auto size = r.renderExtent();
        auto lo = scene.dataset.clusters.front().boundsMin, hi = scene.dataset.clusters.front().boundsMax;
        for (const auto& c : scene.dataset.clusters) {
            lo = glm::min(lo, c.boundsMin);
            hi = glm::max(hi, c.boundsMax);
        }
        const float aspect = float(size.width) / size.height;
        for (uint32_t level = 1; level < scene.hierarchy.levels.size(); ++level) {
            const auto& proxy = scene.hierarchy.levels[level];
            if (!proxy.maxDiameterPixels) continue;
            int worst = 0;
            for (glm::vec2 view : {glm::vec2{0,0}, {.785f,0}, {1.57f,0}, {2.35f,0}, {3.1415f,0},
                                   {-.78f,.42f}, {0,1.46f}, {0,-.75f}}) {
                // Find the entry size by projecting the box, independent of the on-screen triangle count.
                float near = 12, far = 256;
                for (int step = 0; step < 16; ++step) {
                    const float mid = .5f * (near + far);
                    camera.setPose({view, mid});
                    if (projectedDiameterPixels(lo, hi, camera.viewProjection(aspect), size.width, size.height)
                        > .99f * proxy.maxDiameterPixels) near = mid;
                    else far = mid;
                }
                camera.setPose({view, far * .97f});
                s.adaptiveLod = true;
                s.lodPixels = 65536;
                draw();
                expect(r.lodStats().clustersPerLevel[level] == 0, "Distant proxy appeared before its footprint gate.");
                camera.setPose({view, far});
                s.adaptiveLod = false;
                r.setHierarchyLevel(level - 1);
                draw();
                const auto before = r.readCameraDepth();
                s.adaptiveLod = true;
                s.lodPixels = 96;
                draw();
                expect(r.lastRecordedDraw().triangles == proxy.indexCount / 3, "Distance cut did not draw the proxy.");
                const auto after = r.readCameraDepth();
                int delta = 0;
                while (delta <= 2 && !(silhouetteWithin(size, before, after, delta) &&
                                       silhouetteWithin(size, after, before, delta))) ++delta;
                expect(delta <= 2, "Distant LOD lost the silhouette at its entry size.");
                worst = std::max(worst, delta);
            }
            std::cout << "Distant L" << level << ": " << proxy.indexCount / 3 << " triangles, eight views, max switch "
                      << worst << " px.\n";
        }
        return 0;
    }
    auto checkCut = [&](const SceneData& source, float pixels, float padding = 0) {
        s.adaptiveLod = true;
        s.lodPixels = pixels;
        r.setRenderSettings(s);
        prep();
        r.selectLod(camera, padding, true);
        const auto ids = referenceCut(source, camera, r.renderExtent(), pixels, padding);
        const auto& sel = r.clusterSelection();
        expect(ids.size() == sel.ranges.size() && !ids.empty(), "GPU cut has wrong region count.");
        for (size_t i = 0; i < ids.size(); ++i)
            expect(ids[i] == sel.ranges[i].sourceClusterId, "GPU cut differs from root traversal.");
        expect(r.sceneGpuData().lodCommands && r.sceneGpuData().lodTriangleWork &&
                   r.lodStats().gpuMs.has_value() == r.supportsTimestamps(),
               "Missing indirect commands/work or selection timestamp.");
        return sel.triangleCount();
    };
    {
        auto grid = makeTessellationFixture(TessellationFixture::ClusterGrid);
        buildMeshHierarchy(grid, 0);
        expect(grid.hierarchy.levels.size() >= 2, "Expected a parent for the four-cluster test grid.");
        r.uploadScene(grid);
        checkCut(grid, 0);
        expect(r.lodStats().clustersPerLevel.front() == 4, "Strict grid cut must choose all four leaves.");
        checkCut(grid, 65536);
        expect(r.lodStats().clustersPerLevel.front() == 0, "Loose grid cut must choose its root.");
        checkCut(grid, 65536, 1000);
        expect(r.lodStats().clustersPerLevel.front() == 4, "Bounds crossing the near plane must refine to leaves.");
        r.uploadScene(scene);
        s.adaptiveLod = false;
    }
    draw();
    const auto fine = r.readCameraDepth();
    const auto& levels = scene.hierarchy.levels;
    const auto first = levels.front();
    const auto firstDistant = std::find_if(levels.begin(), levels.end(), [](const LodLevel& level) {
        return level.maxDiameterPixels != 0;
    });
    const auto protectedLevel = static_cast<uint32_t>(firstDistant - levels.begin() - 1);
    const auto last = levels[protectedLevel];
    expect(checkCut(scene, 0) == first.indexCount / 3, "Strict threshold did not retain full detail.");
    draw();
    expect(r.readCameraDepth() == fine, "Indirect fine rendering differs from manual L0.");
    expect(checkCut(scene, 65536) == last.indexCount / 3, "Close-up selected a distant proxy or missed a protected root.");
    draw();
    const auto coarse = r.readCameraDepth();
    s.adaptiveLod = false;
    r.setHierarchyLevel(protectedLevel);
    draw();
    expect(coarse == r.readCameraDepth() && coarse != fine, "Root draw geometry diverged from manual coarse level.");
    uint64_t prev = first.indexCount / 3;
    bool mixed = false;
    for (float p : {8.f, 24.f, 48.f, 96.f, 160.f, 256.f, 512.f}) {
        const auto count = checkCut(scene, p);
        expect(count <= prev, "Looser error budget increased triangle count.");
        prev = count;
        const auto& hist = r.lodStats().clustersPerLevel;
        mixed |= std::count_if(hist.begin(), hist.end(), [](auto n) { return n != 0; }) > 1;
    }
    expect(mixed, "No mixed-level frame on Suzanne.");
    const auto baseline = camera;
    camera.zoom(12);
    const auto nearCount = checkCut(scene, 96);
    camera = baseline;
    camera.setPose({baseline.pose().x, baseline.pose().y, 12.f});
    const auto farCount = checkCut(scene, 96);
    expect(nearCount > farCount, "Camera distance did not change actual selected geometry.");
    camera = baseline;
    camera.orbit(61, -32);
    checkCut(scene, 96, .25f);
    camera = baseline;
    const auto selected = checkCut(scene, 96);
    draw();
    const auto selectedDepth = r.readCameraDepth();
    s.levelColors = false;
    s.shadows = true;
    draw();
    expect(r.readCameraDepth() == selectedDepth && r.lastRecordedDraw().triangles == selected &&
               r.lastRecordedDraw().shadowTriangles == selected,
           "Batched shaded and shadow draws did not use the same GPU cut.");
    s.levelColors = true;
    s.shadows = false;
    s.display = RenderDisplay::Depth;
    draw();
    expect(r.readCameraDepth() == selectedDepth, "Depth display changed the underlying camera depth.");
    r.setEdgeSegments(2);
    draw(true);
    r.verifyGeneratedGeometry(scene);
    r.verifyUpload(scene);
    expect(r.lastRecordedDraw().triangles == 4 * selected && !r.ensureGeneratedGeometry(),
           "Adaptive subdivision used different input or lost cache.");
    r.setHeightField(makeHeightField(HeightPreset::White));
    r.setDisplacement({true, .25f});
    checkCut(scene, 96, .25f);
    draw(true);
    r.verifyGeneratedGeometry(scene);
    const auto mixedGeneration = r.generatedGeometry().generationRevision;
    checkCut(scene, 0, .25f);
    draw(true);
    expect(r.generatedGeometry().generationRevision > mixedGeneration,
           "Changed LOD cut reused stale generated geometry.");
    std::cout << "LOD selection at 96 px: near " << nearCount << ", initial " << selected << ", far " << farCount
              << " source triangles.\n";

    r.uploadScene(makeTessellationFixture(TessellationFixture::Plane));
    expect(!r.cameraDepth().view && !r.sceneGpuData().lodCommands,
           "Scene replacement retained old depth or selection.");
    r.selectLod(camera);
    expect(r.sceneGpuData().activeLevel == 0, "Flat fixture must remain on L0.");
    s = {};
    camera = OrbitCamera({0, 0, 0}, 2);
    draw();
    r.checkCameraDepthSampling();
    auto plane = r.readCameraDepth();
    planeDepth(r.cameraDepth(), plane, 0);
    const auto serial = r.cameraDepth().frame;
    s.display = RenderDisplay::Depth;
    draw(true);
    auto shifted = r.readCameraDepth();
    planeDepth(r.cameraDepth(), shifted, .25);
    expect(shifted != plane && r.cameraDepth().frame > serial, "Displacement or frame publication missing.");
    s.display = RenderDisplay::ShadowMap;
    draw();
    expect(!r.cameraDepth().view, "Light debug view published an invalid camera depth.");
    s.display = RenderDisplay::Shaded;
    if (r.supportsWireframe()) {
        draw(false, true);
        expect(!r.cameraDepth().solid, "Wireframe reported solid coverage.");
    }
    glfwSetWindowSize(window, 680, 490);
    glfwPollEvents();
    prep();
    expect(!r.cameraDepth().view, "Resize retained a stale depth handle.");
    camera.orbit(72, 18);
    draw();
    r.checkCameraDepthSampling();
    planeDepth(r.cameraDepth(), r.readCameraDepth(), 0);
    r.setClusterSelection({r.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, {}});
    draw();
    const auto empty = r.readCameraDepth();
    expect(std::all_of(empty.begin(), empty.end(), [](float d) { return d == 1; }),
           "Empty scene retained geometry in its depth.");
    std::cout << "LOD and camera depth passed: independent cuts, mixed IDs, indirect draws/dispatch, camera motion, "
                 "reference depth, resize and empty coverage.\n";
    return 0;
}
