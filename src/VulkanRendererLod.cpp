#include "VulkanRenderer.h"
#include "OrbitCamera.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void checkLod(VkResult r, const char* op)
{
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(op) + " failed: " + std::to_string(r));
}
constexpr uint32_t shaderCode[] = {
#include "select_lod.comp.inc"
};
} // namespace

void VulkanRenderer::createLodResources()
{
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 2;
    layout.pBindings = bindings;
    checkLod(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &lodSetLayout_), "Create LOD descriptor layout");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    checkLod(vkCreateDescriptorPool(device_, &pool, nullptr, &lodPool_), "Create LOD descriptor pool");
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = lodPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &lodSetLayout_;
    checkLod(vkAllocateDescriptorSets(device_, &alloc, &lodSet_), "Allocate LOD descriptors");
    const VkDescriptorSetLayout sets[]{sceneLayout_, lodSetLayout_};
    static_assert(offsetof(LodPush, nodeWord) == 32 && offsetof(LodPush, phase) == 56);
    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LodPush)};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 2;
    pi.pSetLayouts = sets;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &range;
    checkLod(vkCreatePipelineLayout(device_, &pi, nullptr, &lodLayout_), "Create LOD pipeline layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof(shaderCode);
    sm.pCode = shaderCode;
    VkShaderModule module = VK_NULL_HANDLE;
    checkLod(vkCreateShaderModule(device_, &sm, nullptr, &module), "Create LOD shader");
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.layout = lodLayout_;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = module;
    pipeline.stage.pName = "main";
    auto result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &lodPipeline_);
    vkDestroyShaderModule(device_, module, nullptr);
    checkLod(result, "Create LOD pipeline");
    if (supportsTimestamps()) {
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 2;
        checkLod(vkCreateQueryPool(device_, &q, nullptr, &lodQueries_), "Create LOD timestamps");
    }
}

void VulkanRenderer::sizeLodBuffers(VkDeviceSize resultBytes, VkDeviceSize workBytes)
{
    const auto limit = deviceProperties_.limits.maxStorageBufferRange;
    if (resultBytes > limit || workBytes > limit)
        throw std::runtime_error("LOD work exceeds GPU storage-buffer range.");
    if (lodResultSize_ >= resultBytes && lodWorkSize_ >= workBytes) return;
    Buffer result{}, work{};
    try {
        createBuffer(result, resultBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        createBuffer(work, workBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    catch (...) {
        destroyBuffer(result);
        destroyBuffer(work);
        throw;
    }
    const VkDescriptorBufferInfo info[] = {{result.handle, 0, resultBytes}, {work.handle, 0, workBytes}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        auto& w = writes[i];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = lodSet_;
        w.dstBinding = i;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.descriptorCount = 1;
        w.pBufferInfo = &info[i];
    }
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    destroyBuffer(lodResult_);
    destroyBuffer(lodWork_);
    lodResult_ = result;
    lodWork_ = work;
    lodResultSize_ = resultBytes;
    lodWorkSize_ = workBytes;
}

void VulkanRenderer::selectLod(const OrbitCamera& camera, float padding, bool force)
{
    const auto& h = sceneGpu_.hierarchy;
    if (!h.nodeCount) {
        setHierarchyLevel(0);
        return;
    }
    if (!extent_.height || !extent_.width || !std::isfinite(padding) || padding < 0)
        throw std::runtime_error("LOD selection requires a viewport and finite nonnegative padding.");
    const auto start = std::chrono::steady_clock::now();
    const auto aspect = float(extent_.width) / extent_.height;
    const auto vp = camera.viewProjection(aspect);
    LodPush push{};
    push.cameraZ = {vp[0][3], vp[1][3], vp[2][3], vp[3][3]};
    push.focalPixels = camera.focalPixels(extent_.height);
    push.nearPlane = camera.nearPlane(aspect);
    push.budget = renderSettings_.lodPixels;
    push.padding = padding;
    const bool entering = sceneGpu_.activeLevel != kAdaptiveGeometryLevel;
    if (!force && !entering && push.cameraZ == lastLod_.cameraZ && push.focalPixels == lastLod_.focalPixels &&
        push.nearPlane == lastLod_.nearPlane && push.budget == lastLod_.budget && padding == lastLod_.padding)
        return;
    if (entering && sceneGpu_.sceneRevision == UINT64_MAX) throw std::runtime_error("Scene revision exhausted.");
    push.nodes = h.nodeCount;
    push.nodeWord = h.nodeWord;
    push.blocks = (push.nodes - 1) / 64 + 1;
    push.gridX = std::min(push.blocks, deviceProperties_.limits.maxComputeWorkGroupCount[0]);
    const uint32_t gridY = (push.blocks - 1) / push.gridX + 1;
    if (gridY > deviceProperties_.limits.maxComputeWorkGroupCount[1])
        throw std::runtime_error("LOD grid exceeds device limits.");
    push.clusterCapacity = h.totalClusters;
    push.triangleCapacity = h.totalIndices / 3;
    const uint64_t record = 16ull + 3ull * push.nodes + 2ull * push.blocks;
    const uint64_t bytes = 4 * (record + 6ull * push.clusterCapacity);
    checkLod(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX), "Wait for previous frame before LOD");
    sizeLodBuffers(bytes, 4ull * push.triangleCapacity);
    checkLod(vkResetCommandBuffer(commandBuffer_, 0), "Reset LOD command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkLod(vkBeginCommandBuffer(commandBuffer_, &begin), "Begin LOD");
    const auto barrier = [&](VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                             VkAccessFlags dstAccess) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(commandBuffer_, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
    };
    barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (lodQueries_) {
        vkCmdResetQueryPool(commandBuffer_, lodQueries_, 0, 2);
        vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, lodQueries_, 0);
    }
    const VkDescriptorSet sets[]{sceneGpu_.clusterSet, lodSet_};
    vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, lodPipeline_);
    vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, lodLayout_, 0, 2, sets, 0, nullptr);
    for (push.phase = 0; push.phase < 3; ++push.phase) {
        if (push.phase)
            barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdPushConstants(commandBuffer_, lodLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(commandBuffer_, push.phase == 1 ? 1 : push.gridX, push.phase == 1 ? 1 : gridY, 1);
    }
    if (lodQueries_) vkCmdWriteTimestamp(commandBuffer_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, lodQueries_, 1);
    barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_HOST_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    checkLod(vkEndCommandBuffer(commandBuffer_), "End LOD");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    checkLod(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Submit LOD");
    checkLod(vkQueueWaitIdle(graphicsQueue_), "Wait for LOD counts");
    // CPU mirrors compact IDs for UI, bounds, command offsets and exact output allocation.
    // Draw commands, triangle addresses and subdivision dispatch sizes stay GPU-produced.
    ClusterSelection cut{sceneGpu_.sceneRevision + (entering ? 1u : 0u), ClusterSelectionMode::Explicit, {}};
    uint32_t triangles = 0;
    void* mapped = nullptr;
    checkLod(vkMapMemory(device_, lodResult_.memory, 0, bytes, 0, &mapped), "Read LOD counts");
    try {
        const auto* words = static_cast<const uint32_t*>(mapped);
        if (words[2] || words[0] > push.clusterCapacity || words[1] > push.triangleCapacity)
            throw std::runtime_error("GPU LOD cut exceeded its output capacity.");
        triangles = words[1];
        cut.clusterIds.reserve(words[0]);
        for (uint32_t i = 0; i < words[0]; ++i)
            cut.clusterIds.push_back(words[record + 6ull * i]);
    }
    catch (...) {
        vkUnmapMemory(device_, lodResult_.memory);
        throw;
    }
    vkUnmapMemory(device_, lodResult_.memory);
    auto resolved = resolveClusterSelection(residentClusters_, h.totalIndices, cut.sceneRevision, cut);
    if (resolved.triangleCount() != triangles)
        throw std::runtime_error("GPU LOD triangle count disagrees with its cluster IDs.");
    LodSelectionStats stats;
    stats.clustersPerLevel.resize(residentLevels_.size());
    for (auto id : cut.clusterIds)
        ++stats.clustersPerLevel[clusterLevels_[id]];
    if (lodQueries_) {
        uint64_t stamps[2]{};
        checkLod(vkGetQueryPoolResults(device_, lodQueries_, 0, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                 "Read LOD timestamps");
        const auto mask = timestampValidBits_ == 64 ? UINT64_MAX : (uint64_t{1} << timestampValidBits_) - 1;
        stats.gpuMs = double((stamps[1] - stamps[0]) & mask) * timestampPeriod_ / 1.e6;
    }
    if (entering) {
        drawClusters_ = residentClusters_;
        sceneGpu_.sceneRevision = cut.sceneRevision;
        sceneGpu_.activeLevel = kAdaptiveGeometryLevel;
        sceneGpu_.firstCluster = 0;
        sceneGpu_.indexOffset = h.allIndexOffset;
        sceneGpu_.clusterCount = h.totalClusters;
        sceneGpu_.indexCount = h.totalIndices;
    }
    sceneGpu_.lodCommands = lodResult_.handle;
    sceneGpu_.lodTriangleWork = lodWork_.handle;
    sceneGpu_.lodRecordOffset = 4 * record;
    sceneGpu_.lodWorkAllocation = lodWork_.allocationBytes;
    if (!sameClusterSelection(selection_, resolved)) compute_->invalidate();
    selection_ = std::move(resolved);
    lastLod_ = push;
    stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    lodStats_ = std::move(stats);
}
