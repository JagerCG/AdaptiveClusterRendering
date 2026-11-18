#include "VulkanRenderer.h"
#include "OrbitCamera.h"
#include "TessellationFixtures.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool ok, const char* what)
{
    if (!ok) throw std::runtime_error(what);
}

std::vector<uint32_t> ids(const ResolvedClusterSelection& s)
{
    std::vector<uint32_t> out;
    for (const auto& range : s.ranges)
        out.push_back(range.sourceClusterId);
    return out;
}

void checkMips(uint32_t width, uint32_t height, std::vector<float> src, const std::vector<float>& actual)
{
    size_t offset = 0;
    for (;;) {
        expect(offset + src.size() <= actual.size() && std::equal(src.begin(), src.end(), actual.begin() + offset),
               "Hi-Z pixels differ from exact CPU max reduction.");
        offset += src.size();
        if (width == 1 && height == 1) break;
        const uint32_t w = (width + 1) / 2, h = (height + 1) / 2;
        std::vector<float> next(size_t(w) * h);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                auto& v = next[size_t(y / 2) * w + x / 2];
                v = std::max(v, src[size_t(y) * width + x]);
            }
        src = std::move(next);
        width = w;
        height = h;
    }
    expect(offset == actual.size(), "Unexpected pyramid length.");
}
} // namespace

std::vector<float> VulkanRenderer::readHiZ()
{
    expect(cameraList_.active && cameraList_.stats.depthFrame && !hiZMips_.empty(), "No valid GPU depth pyramid.");
    const auto last = hiZMips_.back();
    std::vector<float> data(size_t(last.offset) + last.width * last.height);
    const auto bytes = data.size() * sizeof(float);
    expect(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX) == VK_SUCCESS,
           "Wait for pyramid readback.");
    Buffer staging{};
    try {
        createBuffer(staging, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        expect(vkResetCommandBuffer(commandBuffer_, 0) == VK_SUCCESS, "Reset pyramid readback.");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        expect(vkBeginCommandBuffer(commandBuffer_, &begin) == VK_SUCCESS, "Begin pyramid readback.");
        const VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(commandBuffer_, hiZ_.handle, staging.handle, 1, &copy);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &barrier, 0, nullptr, 0, nullptr);
        expect(vkEndCommandBuffer(commandBuffer_) == VK_SUCCESS, "End pyramid readback.");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        expect(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                   vkQueueWaitIdle(graphicsQueue_) == VK_SUCCESS,
               "Submit pyramid readback.");
        void* mapped{};
        expect(vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped) == VK_SUCCESS, "Map pyramid readback.");
        std::memcpy(data.data(), mapped, bytes);
        vkUnmapMemory(device_, staging.memory);
    }
    catch (...) {
        destroyBuffer(staging);
        throw;
    }
    destroyBuffer(staging);
    return data;
}

int runVisibilityChecks(VulkanRenderer& r, GLFWwindow* window, const SceneData& suzanne, OrbitCamera suzanneCamera)
{
    const auto stage = makeTessellationFixture(TessellationFixture::Occlusion);
    r.uploadScene(stage);
    OrbitCamera camera({0, 0, 0}, 2);
    camera.setPose({0, 0, 3});
    RenderSettings s;
    const auto draw = [&](bool generated = false, bool wire = false) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            glfwPollEvents();
            if (!r.prepareFrame(window)) continue;
            r.setRenderSettings(s);
            if (s.adaptiveLod) r.selectLod(camera, generated ? r.plannedDisplacementBound() : 0);
            r.selectVisibility(camera, generated, wire);
            if (generated) r.ensureGeneratedGeometry();
            if (r.drawFrame(camera, false, false, -1, generated, wire, nullptr)) return;
        } while (std::chrono::steady_clock::now() < until);
        throw std::runtime_error("Visibility test frame timed out.");
    };
    draw();
    const auto full = r.readCameraDepth();
    expect(r.lastRecordedDraw().triangles == 32, "Expected 32 source triangles in the occlusion stage.");
    s.culling = CullMode::Frustum;
    draw();
    const auto direct = ids(r.mainSelection());
    RenderSettings cpu;
    cpu.frustumPreview = true;
    std::vector<uint32_t> expected;
    const auto size = r.renderExtent();
    const float aspect = float(size.width) / size.height;
    for (uint32_t id = 0; id < stage.dataset.clusters.size(); ++id)
        if (clusterVisible(stage.dataset.clusters[id], 0, camera.viewProjection(aspect), camera.eyePosition(aspect),
                           cpu))
            expected.push_back(id);
    expect(direct == expected && direct.size() == 12 && r.visibilityStats().frustum == 4,
           "GPU frustum differs from CPU or authored off-screen panel.");
    s.culling = CullMode::Hierarchy;
    draw();
    expect(ids(r.mainSelection()) == direct && r.visibilityStats().hierarchy == 4 &&
               r.visibilityStats().skippedNodes > 0,
           "Hierarchy result or parent rejection propagation incorrect.");
    s.culling = CullMode::HiZ;
    s.showHiZ = true;
    s.hiZMip = 3;
    draw();
    const auto visible = ids(r.mainSelection());
    expect(r.visibilityStats().depthFrame && r.visibilityStats().occluded >= 4 && visible.size() < direct.size(),
           "Hi-Z did not remove the hidden panel.");
    for (uint32_t id = 4; id < 8; ++id)
        expect(std::find(visible.begin(), visible.end(), id) == visible.end(), "Hidden panel survived.");
    expect(std::find(visible.begin(), visible.end(), 9) != visible.end(), "Partly visible panel disappeared.");
    expect(full == r.readCameraDepth(), "Source culling changed a camera-depth pixel.");
    checkMips(size.width, size.height, full, r.readHiZ());
    s.culling = CullMode::FlatHiZ;
    draw();
    expect(ids(r.mainSelection()) == visible && r.visibilityStats().depthFrame &&
               r.visibilityStats().testedNodes == 0 && r.visibilityStats().frustum == 4 &&
               r.visibilityStats().occluded == 4 && r.readCameraDepth() == full,
           "Per-cluster Hi-Z must operate independently of hierarchy traversal.");
    s.culling = CullMode::HiZ;
    s.showHiZ = false;
    s.showRejectedBounds = true;
    draw();
    expect(r.lastRecordedDraw().rejectedBoundsInstances == 16 - visible.size() && r.readCameraDepth() == full,
           "Rejected bounds overlay changed scene depth or missed rejected IDs.");
    s.showRejectedBounds = false;
    std::cout << "Occlusion stage: 16 candidates, " << direct.size() << " inside frustum, " << visible.size()
              << " survive Hi-Z, " << r.lastRecordedDraw().triangles << " source triangles.\n";

    s.showHiZ = false;
    s.culling = CullMode::Off;
    s.shadows = true;
    r.setEdgeSegments(2);
    r.setDisplacement({true, .08f});
    draw(true);
    const auto displaced = r.readCameraDepth(), shadow = r.readShadowDepth();
    s.culling = CullMode::HiZ;
    draw(true);
    expect(r.visibilityStats().occluded > 0 &&
               r.lastRecordedDraw().triangles == r.mainSelection().triangleCount() * 4 &&
               r.lastRecordedDraw().shadowTriangles == 128,
           "Visible subdivision or independent generated shadow list incorrect.");
    expect(displaced == r.readCameraDepth() && shadow == r.readShadowDepth(),
           "Culling changed a visible or light-space surface.");
    r.verifyGeneratedGeometry(stage);
    r.setHeightField(makeHeightField(HeightPreset::White));
    r.setDisplacement({true, .5f});
    draw(true);
    expect(!r.visibilityStats().depthFrame, "Changed displaced surface reused old depth.");
    draw(true);
    expect(r.visibilityStats().depthFrame && r.lastRecordedDraw().boundsPadding == r.plannedDisplacementBound(),
           "Displacement bounds handoff is wrong.");
    const auto padded = r.readCameraDepth();
    s.culling = CullMode::Off;
    draw(true);
    expect(padded == r.readCameraDepth(), "Expanded bounds lost displaced geometry.");
    r.setDisplacement({false, 0});
    s.shadows = false;
    s.culling = CullMode::HiZ;
    camera.setPose({3.14159265f, 0, 3});
    draw();
    expect(!r.visibilityStats().depthFrame, "Camera rotation reused a stale pyramid.");
    draw();
    const auto reverse = r.readCameraDepth();
    const auto reversedIDs = ids(r.mainSelection());
    expect(std::find(reversedIDs.begin(), reversedIDs.end(), 5) != reversedIDs.end(),
           "Newly revealed panel was still hidden.");
    s.culling = CullMode::Off;
    draw();
    expect(reverse == r.readCameraDepth(), "Rotated frame differs from full drawing.");

    camera.setPose({0, 0, 3});
    r.setClusterSelection({r.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, {9, 4, 0}});
    s.culling = CullMode::HiZ;
    draw();
    expect(!r.visibilityStats().depthFrame, "Changed candidate list reused old depth.");
    expect(ids(r.mainSelection()) == std::vector<uint32_t>({9, 4, 0}), "Culling reordered manual candidates.");
    r.setClusterSelection({r.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, {15, 12}});
    s.shadows = true;
    draw(true);
    expect(r.visibilityStats().candidates == 2 && r.mainSelection().ranges.empty() &&
               !r.lastRecordedDraw().triangles && r.lastRecordedDraw().shadowTriangles == 16,
           "A fully rejected main list incorrectly suppressed its shadow casters.");
    s.shadows = false;
    r.setClusterSelection({r.sceneGpuData().sceneRevision, ClusterSelectionMode::Explicit, {}});
    draw(true);
    expect(!r.lastRecordedDraw().triangles && !r.generatedGeometry().indexCount, "Empty GPU list produced geometry.");
    r.verifyGeneratedGeometry(stage);
    r.setClusterSelection({r.sceneGpuData().sceneRevision, ClusterSelectionMode::All, {}});
    glfwSetWindowSize(window, 333, 257);
    draw();
    expect(!r.visibilityStats().depthFrame, "Resized viewport reused old depth.");
    const auto odd = r.readCameraDepth();
    draw();
    expect(r.renderExtent().width % 2 && r.renderExtent().height % 2, "Expected an odd-sized viewport.");
    checkMips(r.renderExtent().width, r.renderExtent().height, odd, r.readHiZ());
    if (r.supportsWireframe()) {
        draw(false, true);
        expect(!r.visibilityStats().depthFrame, "Wireframe used solid occlusion.");
    }
    s.display = RenderDisplay::ShadowMap;
    draw();
    expect(!r.visibilityStats().depthFrame && !r.cameraDepth().view, "Light-only view provided main depth.");

    r.uploadScene(makeTessellationFixture(TessellationFixture::ClusterGrid));
    s = {};
    s.culling = CullMode::HiZ;
    camera.setPose({0, 0, 3});
    draw();
    draw();
    expect(r.visibilityStats().depthFrame && r.visibilityStats().testedNodes == 0 &&
               r.mainSelection().ranges.size() == 4,
           "Flat legacy cluster data must work without hierarchy nodes.");
    // The raw triangle lies beyond the right plane. White height moves it left into view.
    auto entering = makeTessellationFixture(TessellationFixture::Triangle);
    for (auto& v : entering.mesh.vertices) {
        v.position = v.position * .25f + glm::vec3(4.1f, 0, 0);
        v.normal = {-1, 0, 0};
    }
    entering.dataset.clusters[0].boundsMin = {3.85f, -.25f, 0};
    entering.dataset.clusters[0].boundsMax = {4.35f, .25f, 0};
    r.uploadScene(entering);
    draw();
    expect(r.mainSelection().ranges.empty(), "Undisplaced test triangle should be outside the right plane.");
    r.setDisplacement({true, 1.5f});
    s.culling = CullMode::Off;
    draw(true);
    const auto moved = r.readCameraDepth();
    expect(*std::min_element(moved.begin(), moved.end()) < 1, "Displaced triangle did not enter the viewport.");
    s.culling = CullMode::HiZ;
    draw(true);
    expect(r.mainSelection().ranges.size() == 1 && r.readCameraDepth() == moved,
           "Culling lost a triangle displaced into view.");
    r.setDisplacement({false, 0});
    r.uploadScene(suzanne);
    camera = suzanneCamera;
    s = {};
    r.setHierarchyLevel(1);
    r.setEdgeSegments(2);
    draw(true);
    const auto manual = r.readCameraDepth();
    s.culling = CullMode::HiZ;
    draw(true);
    r.verifyGeneratedGeometry(suzanne);
    expect(manual == r.readCameraDepth(), "Manual coarse index base changed camera pixels.");
    s.culling = CullMode::Off;
    s.adaptiveLod = true;
    s.lodPixels = 96;
    draw(true);
    const auto adaptive = r.readCameraDepth();
    const auto candidates = ids(r.clusterSelection());
    s.culling = CullMode::HiZ;
    draw(true);
    const auto survivors = ids(r.mainSelection());
    expect(r.visibilityStats().depthFrame &&
               std::includes(candidates.begin(), candidates.end(), survivors.begin(), survivors.end()) &&
               r.lastRecordedDraw().triangles == r.mainSelection().triangleCount() * 4 &&
               r.readCameraDepth() == adaptive,
           "LOD, GPU culling and generated rasterization disagree.");
    r.verifyGeneratedGeometry(suzanne);
    std::cout << "Visibility integration passed: exact camera/shadow depth, independent max mips, ordered lists, "
                 "displacement, resize, disocclusion and real mixed LOD.\n";
    return 0;
}
