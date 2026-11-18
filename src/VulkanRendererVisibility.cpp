#include "VulkanRenderer.h"
#include "OrbitCamera.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
void check(VkResult result, const char* action)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + " failed (" + std::to_string(result) + ")");
}
constexpr uint32_t cullCode[] = {
#include "cull_visibility.comp.inc"
};
constexpr uint32_t pyramidCode[] = {
#include "build_hiz.comp.inc"
};
constexpr auto stages = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
struct CullParams {
    glm::mat4 vp;
    uint32_t candidates, firstCluster, firstIndex, inputBase;
    uint32_t stride, nodeWord, nodes, useHierarchy;
    uint32_t phase, firstNode, count, gridX;
    uint32_t width, height, useDepth;
    float padding;
};
static_assert(sizeof(CullParams) == 128 && offsetof(CullParams, padding) == 124);
void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags src, VkPipelineStageFlags to,
                   VkAccessFlags dst)
{
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace

void VulkanRenderer::createVisibilityResources()
{
    VkDescriptorSetLayoutBinding bindings[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorCount = 1;
        bindings[i].descriptorType =
            i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].stageFlags = i == 0 ? stages : VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 5;
    si.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &si, nullptr, &visSetLayout_), "Visibility descriptor layout");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    pi.maxSets = 2;
    check(vkCreateDescriptorPool(device_, &pi, nullptr, &visPool_), "Visibility descriptor pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = visPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &visSetLayout_;
    for (auto* list : {&cameraList_, &lightList_})
        check(vkAllocateDescriptorSets(device_, &ai, &list->set), "Visibility descriptor set");
    const VkDescriptorSetLayout layouts[] = {sceneLayout_, visSetLayout_};
    const VkPushConstantRange range{stages, 0, sizeof(CullParams)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = layouts;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device_, &li, nullptr, &visLayout_), "Visibility pipeline layout");
    const auto pipeline = [&](const uint32_t* words, size_t bytes, VkPipeline& target) {
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.pCode = words;
        sm.codeSize = bytes;
        VkShaderModule module{};
        check(vkCreateShaderModule(device_, &sm, nullptr, &module), "Visibility shader");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = visLayout_;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        auto rc = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &target);
        vkDestroyShaderModule(device_, module, nullptr);
        check(rc, "Visibility compute pipeline");
    };
    pipeline(cullCode, sizeof(cullCode), visCull_);
    pipeline(pyramidCode, sizeof(pyramidCode), hiZBuild_);
    if (supportsTimestamps()) {
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 3;
        check(vkCreateQueryPool(device_, &q, nullptr, &visQueries_), "Visibility timestamps");
    }
}

void VulkanRenderer::destroyVisibilityResources()
{
    destroyBuffer(hiZ_);
    destroyBuffer(visInput_);
    for (auto* list : {&cameraList_, &lightList_}) {
        destroyBuffer(list->result);
        destroyBuffer(list->work);
    }
    vkDestroyQueryPool(device_, visQueries_, nullptr);
    vkDestroyPipeline(device_, visCull_, nullptr);
    vkDestroyPipeline(device_, hiZBuild_, nullptr);
    vkDestroyPipelineLayout(device_, visLayout_, nullptr);
    vkDestroyDescriptorPool(device_, visPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, visSetLayout_, nullptr);
}

void VulkanRenderer::growVisibilityBuffer(Buffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes,
                                          VkBufferUsageFlags usage, VkMemoryPropertyFlags memory)
{
    if (capacity >= bytes) return;
    if (bytes > deviceProperties_.limits.maxStorageBufferRange)
        throw std::runtime_error("Visibility data exceeds the device storage-buffer range.");
    Buffer replacement{};
    try {
        createBuffer(replacement, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | usage, memory);
    }
    catch (...) {
        destroyBuffer(replacement);
        throw;
    }
    destroyBuffer(buffer);
    buffer = replacement;
    capacity = bytes;
}

void VulkanRenderer::selectVisibility(const OrbitCamera& camera, bool generated, bool wireframe)
{
    cameraList_.active = false;
    cameraList_.stats = {};
    hiZMips_.clear();
    const auto mode = renderSettings_.culling;
    if (mode == CullMode::Off) return;
    const auto vp = camera.viewProjection(float(extent_.width) / extent_.height);
    const bool useDepth = usesHiZ(mode) && !wireframe && renderSettings_.display != RenderDisplay::ShadowMap &&
                          !renderSettings_.frustumPreview && renderSettings_.maxDistance == 0 && cameraDepth_.view &&
                          cameraDepth_.solid && cameraDepth_.sceneRevision == sceneGpu_.sceneRevision &&
                          cameraDepth_.extent.width == extent_.width && cameraDepth_.extent.height == extent_.height &&
                          cameraDepth_.viewProjection == vp && depthInputs_.candidates == candidateRevision_ &&
                          depthInputs_.generated == generated &&
                          (!generated || (depthInputs_.segments == edgeSegments_ && depthInputs_.height == heightRevision() &&
                                          sameDisplacement(depthInputs_.displacement, displacement_)));
    buildVisibilityList(cameraList_, vp, extent_, generated ? plannedDisplacementBound() : 0,
                        mode == CullMode::Hierarchy || mode == CullMode::HiZ, useDepth ? cameraDepth_.frame : 0);
}

void VulkanRenderer::buildVisibilityList(VisibilityList& list, const glm::mat4& vp, VkExtent2D extent, float padding,
                                         bool hierarchy, uint64_t depthFrame)
{
    list.active = false;
    list.stats = {};
    const auto start = std::chrono::steady_clock::now();
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX), "Wait before culling");
    const auto& h = sceneGpu_.hierarchy;
    CullParams p{};
    p.vp = vp;
    p.candidates = static_cast<uint32_t>(selection_.ranges.size());
    p.firstCluster = sceneGpu_.firstCluster;
    p.firstIndex = static_cast<uint32_t>((sceneGpu_.indexOffset - h.allIndexOffset) / sizeof(uint32_t));
    p.inputBase = static_cast<uint32_t>(sceneGpu_.lodRecordOffset / 4);
    p.stride = sceneGpu_.lodCommands ? 6 : 1;
    p.nodeWord = h.nodeWord;
    p.nodes = hierarchy ? h.nodeCount : 0;
    p.useHierarchy = p.nodes != 0;
    p.width = extent.width;
    p.height = extent.height;
    p.padding = padding;
    p.useDepth = depthFrame != 0;
    uint64_t pyramidWords = 0;
    if (p.useDepth) {
        for (uint32_t w = p.width, hgt = p.height;; w = (w + 1) / 2, hgt = (hgt + 1) / 2) {
            hiZMips_.push_back({w, hgt, static_cast<uint32_t>(pyramidWords)});
            pyramidWords += uint64_t(w) * hgt;
            if (pyramidWords > deviceProperties_.limits.maxStorageBufferRange / 4)
                throw std::runtime_error("Camera depth pyramid exceeds the device storage-buffer range.");
            if (w == 1 && hgt == 1) break;
        }
    }
    const uint32_t blocks = (uint64_t(p.candidates) + 63) / 64;
    const uint64_t record = 24ull + p.nodes + 3ull * p.candidates + 2ull * blocks;
    const uint64_t resultBytes = (record + 6ull * p.candidates) * 4;
    growVisibilityBuffer(hiZ_, hiZCapacity_, std::max<uint64_t>(4, 4 * pyramidWords),
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    growVisibilityBuffer(list.result, list.resultCapacity, resultBytes,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, coherent);
    growVisibilityBuffer(list.work, list.workCapacity, std::max<uint64_t>(4, 4 * selection_.triangleCount()), 0,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!sceneGpu_.lodCommands) {
        growVisibilityBuffer(visInput_, visInputCapacity_, std::max<uint64_t>(4, 4ull * p.candidates), 0, coherent);
        if (p.candidates) {
            void* mapped = nullptr;
            check(vkMapMemory(device_, visInput_.memory, 0, 4ull * p.candidates, 0, &mapped),
                  "Map manual candidates");
            auto* ids = static_cast<uint32_t*>(mapped);
            for (size_t i = 0; i < p.candidates; ++i)
                ids[i] = selection_.ranges[i].sourceClusterId;
            vkUnmapMemory(device_, visInput_.memory);
        }
    }
    const VkDescriptorBufferInfo buffers[] = {{hiZ_.handle, 0, hiZCapacity_},
                                              {},
                                              {sceneGpu_.lodCommands ? sceneGpu_.lodCommands : visInput_.handle, 0,
                                               sceneGpu_.lodCommands ? lodResultSize_ : visInputCapacity_},
                                              {list.result.handle, 0, resultBytes},
                                              {list.work.handle, 0, list.workCapacity}};
    const VkDescriptorImageInfo image{shadowSampler_, depthFrame ? depthView_ : shadowView_,
                                      depthFrame ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        auto& w = writes[i];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = list.set;
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &buffers[i];
        w.pImageInfo = &image;
    }
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
    auto cmd = commandBuffer_;
    check(vkResetCommandBuffer(cmd, 0), "Reset visibility commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "Begin visibility commands");
    memoryBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdFillBuffer(cmd, list.result.handle, 0, 24 * 4, 0);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const VkDescriptorSet sets[]{sceneGpu_.clusterSet, list.set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visLayout_, 0, 2, sets, 0, nullptr);
    if (visQueries_) {
        vkCmdResetQueryPool(cmd, visQueries_, 0, 3);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, visQueries_, 0);
    }
    const auto computeBarrier = [&] {
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    if (p.useDepth) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZBuild_);
        for (size_t i = 0; i < hiZMips_.size(); ++i) {
            const auto src = hiZMips_[i ? i - 1 : 0], dst = hiZMips_[i];
            const uint32_t args[] = {src.width, src.height, src.offset, 0,
                                     dst.width, dst.height, dst.offset, uint32_t(i == 0)};
            vkCmdPushConstants(cmd, visLayout_, stages, 0, sizeof(args), args);
            vkCmdDispatch(cmd, (dst.width + 7) / 8, (dst.height + 7) / 8, 1);
            computeBarrier();
        }
    }
    if (visQueries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, visQueries_, 1);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, visCull_);
    const auto dispatch = [&](uint32_t phase, uint32_t groups) {
        if (!groups) return;
        p.phase = phase;
        p.gridX = std::min(groups, deviceProperties_.limits.maxComputeWorkGroupCount[0]);
        const uint32_t rows = (groups - 1) / p.gridX + 1;
        if (rows > deviceProperties_.limits.maxComputeWorkGroupCount[1])
            throw std::runtime_error("Visibility grid exceeds device limits.");
        vkCmdPushConstants(cmd, visLayout_, stages, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, p.gridX, rows, 1);
        computeBarrier();
    };
    if (p.useHierarchy && p.candidates) {
        for (auto it = residentLevels_.rbegin(); it != residentLevels_.rend(); ++it) {
            p.firstNode = it->firstNode;
            p.count = it->nodeCount;
            dispatch(0, (p.count - 1) / 64 + 1);
        }
    }
    dispatch(1, blocks);
    dispatch(2, 1);
    dispatch(3, blocks);
    if (visQueries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, visQueries_, 2);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_HOST_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT);
    check(vkEndCommandBuffer(cmd), "End visibility commands");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Submit visibility");
    check(vkQueueWaitIdle(graphicsQueue_), "Wait for visibility counts");
    ClusterSelection surviving{sceneGpu_.sceneRevision, ClusterSelectionMode::Explicit, {}};
    surviving.clusterIds.reserve(p.candidates);
    void* mapped = nullptr;
    check(vkMapMemory(device_, list.result.memory, 0, resultBytes, 0, &mapped), "Map visibility result");
    const auto* words = static_cast<const uint32_t*>(mapped);
    list.stats.candidates = p.candidates;
    list.stats.visible = words[0];
    list.stats.frustum = words[17];
    list.stats.hierarchy = words[18];
    list.stats.occluded = words[19] + words[20];
    list.stats.testedNodes = words[21];
    list.stats.skippedNodes = words[22];
    const uint32_t triangles = words[1];
    if (words[0] <= p.candidates)
        for (uint32_t i = 0; i < words[0]; ++i)
            surviving.clusterIds.push_back(words[record + 6ull * i]);
    vkUnmapMemory(device_, list.result.memory);
    if (uint64_t(list.stats.visible) + list.stats.frustum + list.stats.hierarchy + list.stats.occluded != p.candidates)
        throw std::runtime_error("GPU visibility reason counts do not cover the candidate list.");
    list.selection =
        resolveClusterSelection(drawClusters_, sceneGpu_.indexCount, sceneGpu_.sceneRevision, surviving);
    if (list.selection.triangleCount() != triangles)
        throw std::runtime_error("GPU visible geometry count disagrees with its IDs.");
    list.scene = sceneGpu_;
    list.scene.lodCommands = list.result.handle;
    list.scene.lodRecordOffset = record * 4;
    list.scene.lodTriangleWork = list.work.handle;
    list.scene.lodWorkAllocation = list.work.allocationBytes;
    list.stats.mipCount = depthFrame ? static_cast<uint32_t>(hiZMips_.size()) : 0;
    list.stats.depthFrame = depthFrame;
    if (visQueries_) {
        uint64_t t[3];
        check(vkGetQueryPoolResults(device_, visQueries_, 0, 3, sizeof(t), t, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "Visibility timestamps");
        const uint64_t mask = timestampValidBits_ == 64 ? UINT64_MAX : (uint64_t{1} << timestampValidBits_) - 1;
        const auto ms = [&](int a, int b) { return double((t[b] - t[a]) & mask) * timestampPeriod_ / 1.e6; };
        list.stats.gpuCullMs = ms(1, 2);
        if (p.useDepth) list.stats.gpuPyramidMs = ms(0, 1);
    }
    list.stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    list.active = true;
}
