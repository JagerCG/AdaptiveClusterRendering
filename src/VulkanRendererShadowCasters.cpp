#include "VulkanRenderer.h"
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
constexpr uint32_t casterCode[] = {
#include "select_shadow_casters.comp.inc"
};
constexpr auto stages = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr auto coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
struct CasterParams {
    glm::mat4 vp;
    uint32_t candidates, firstCluster, firstIndex, phase;
    uint32_t gridX;
    float padding;
};
static_assert(sizeof(CasterParams) == 88 && offsetof(CasterParams, padding) == 84);
void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags src, VkPipelineStageFlags to,
                   VkAccessFlags dst)
{
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cmd, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
}
} // namespace

void VulkanRenderer::createShadowCasterResources()
{
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 3;
    si.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &si, nullptr, &casterSetLayout_), "Shadow caster descriptor layout");
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    pi.maxSets = 1;
    check(vkCreateDescriptorPool(device_, &pi, nullptr, &casterPool_), "Shadow caster descriptor pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = casterPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &casterSetLayout_;
    check(vkAllocateDescriptorSets(device_, &ai, &lightList_.set), "Shadow caster descriptor set");
    const VkDescriptorSetLayout layouts[] = {sceneLayout_, casterSetLayout_};
    const VkPushConstantRange range{stages, 0, sizeof(CasterParams)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 2;
    li.pSetLayouts = layouts;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device_, &li, nullptr, &casterLayout_), "Shadow caster pipeline layout");
    const auto pipeline = [&](const uint32_t* words, size_t bytes, VkPipeline& target) {
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.pCode = words;
        sm.codeSize = bytes;
        VkShaderModule module{};
        check(vkCreateShaderModule(device_, &sm, nullptr, &module), "Shadow caster shader");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = casterLayout_;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        auto rc = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &target);
        vkDestroyShaderModule(device_, module, nullptr);
        check(rc, "Shadow caster compute pipeline");
    };
    pipeline(casterCode, sizeof(casterCode), casterPipeline_);
    if (supportsTimestamps()) {
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        q.queryType = VK_QUERY_TYPE_TIMESTAMP;
        q.queryCount = 2;
        check(vkCreateQueryPool(device_, &q, nullptr, &casterQueries_), "Shadow caster timestamps");
    }
}

void VulkanRenderer::destroyShadowCasterResources()
{
    destroyBuffer(casterInput_);
    destroyBuffer(lightList_.result);
    destroyBuffer(lightList_.work);
    vkDestroyQueryPool(device_, casterQueries_, nullptr);
    vkDestroyPipeline(device_, casterPipeline_, nullptr);
    vkDestroyPipelineLayout(device_, casterLayout_, nullptr);
    vkDestroyDescriptorPool(device_, casterPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, casterSetLayout_, nullptr);
}

void VulkanRenderer::growCasterBuffer(Buffer& buffer, VkDeviceSize& capacity, VkDeviceSize bytes,
                                          VkBufferUsageFlags usage, VkMemoryPropertyFlags memory)
{
    if (capacity >= bytes) return;
    if (bytes > deviceProperties_.limits.maxStorageBufferRange)
        throw std::runtime_error("Shadow caster data exceeds the device storage-buffer range.");
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

void VulkanRenderer::selectShadowCasters(const glm::mat4& vp, float padding)
{
    auto& list = lightList_;
    const auto& candidates = selection_;
    const auto& source = sceneGpu_;
    list.active = false;
    list.stats = {};
    const auto start = std::chrono::steady_clock::now();
    check(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX), "Wait before culling");
    const auto& h = source.hierarchy;
    CasterParams p{};
    p.vp = vp;
    p.candidates = static_cast<uint32_t>(candidates.ranges.size());
    p.firstCluster = source.firstCluster;
    p.firstIndex = static_cast<uint32_t>((source.indexOffset - h.allIndexOffset) / sizeof(uint32_t));
    p.padding = padding;
    const uint32_t blocks = (uint64_t(p.candidates) + 63) / 64;
    const uint64_t record = 24ull + 3ull * p.candidates + 2ull * blocks;
    const uint64_t resultBytes = (record + 6ull * p.candidates) * 4;
    growCasterBuffer(list.result, list.resultCapacity, resultBytes,
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, coherent);
    growCasterBuffer(list.work, list.workCapacity, std::max<uint64_t>(4, 4 * candidates.triangleCount()), 0,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    growCasterBuffer(casterInput_, casterInputCapacity_, std::max<uint64_t>(4, 4ull * p.candidates), 0, coherent);
    if (p.candidates) {
        void* mapped = nullptr;
        check(vkMapMemory(device_, casterInput_.memory, 0, 4ull * p.candidates, 0, &mapped),
              "Map shadow caster candidates");
        auto* ids = static_cast<uint32_t*>(mapped);
        for (size_t i = 0; i < p.candidates; ++i)
            ids[i] = candidates.ranges[i].sourceClusterId;
        vkUnmapMemory(device_, casterInput_.memory);
    }
    const VkDescriptorBufferInfo buffers[] = {{casterInput_.handle, 0, casterInputCapacity_},
                                              {list.result.handle, 0, resultBytes},
                                              {list.work.handle, 0, list.workCapacity}};
    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        auto& w = writes[i];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = list.set;
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(device_, 3, writes, 0, nullptr);
    auto cmd = commandBuffer_;
    check(vkResetCommandBuffer(cmd, 0), "Reset shadow caster commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "Begin shadow caster commands");
    memoryBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdFillBuffer(cmd, list.result.handle, 0, 24 * 4, 0);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    const VkDescriptorSet sets[]{source.clusterSet, list.set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, casterLayout_, 0, 2, sets, 0, nullptr);
    if (casterQueries_) {
        vkCmdResetQueryPool(cmd, casterQueries_, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, casterQueries_, 0);
    }
    const auto computeBarrier = [&] {
        memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, casterPipeline_);
    const auto dispatch = [&](uint32_t phase, uint32_t groups) {
        if (!groups) return;
        p.phase = phase;
        p.gridX = std::min(groups, deviceProperties_.limits.maxComputeWorkGroupCount[0]);
        const uint32_t rows = (groups - 1) / p.gridX + 1;
        if (rows > deviceProperties_.limits.maxComputeWorkGroupCount[1])
            throw std::runtime_error("Shadow caster grid exceeds device limits.");
        vkCmdPushConstants(cmd, casterLayout_, stages, 0, sizeof(p), &p);
        vkCmdDispatch(cmd, p.gridX, rows, 1);
        computeBarrier();
    };
    dispatch(1, blocks);
    dispatch(2, 1);
    dispatch(3, blocks);
    if (casterQueries_) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, casterQueries_, 1);
    memoryBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_HOST_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                      VK_ACCESS_TRANSFER_READ_BIT);
    check(vkEndCommandBuffer(cmd), "End shadow caster commands");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Submit shadow caster");
    check(vkQueueWaitIdle(graphicsQueue_), "Wait for shadow caster counts");
    ClusterSelection surviving{source.sceneRevision, ClusterSelectionMode::Explicit, {}};
    surviving.clusterIds.reserve(p.candidates);
    void* mapped = nullptr;
    check(vkMapMemory(device_, list.result.memory, 0, resultBytes, 0, &mapped), "Map shadow caster result");
    const auto* words = static_cast<const uint32_t*>(mapped);
    list.stats.candidates = p.candidates;
    list.stats.visible = words[0];
    list.stats.frustum = words[17];
    const uint32_t triangles = words[1];
    if (words[0] <= p.candidates)
        for (uint32_t i = 0; i < words[0]; ++i)
            surviving.clusterIds.push_back(words[record + 6ull * i]);
    vkUnmapMemory(device_, list.result.memory);
    if (uint64_t(list.stats.visible) + list.stats.frustum != p.candidates)
        throw std::runtime_error("Shadow caster counts do not cover the candidate list.");
    list.selection =
        resolveClusterSelection(drawClusters_, source.indexCount, source.sceneRevision, surviving);
    if (list.selection.triangleCount() != triangles)
        throw std::runtime_error("GPU visible geometry count disagrees with its IDs.");
    list.scene = source;
    list.scene.indirectCommands = list.result.handle;
    list.scene.drawRecordOffset = record * 4;
    list.scene.triangleWork = list.work.handle;
    list.scene.triangleWorkAllocation = list.work.allocationBytes;
    if (casterQueries_) {
        uint64_t t[2];
        check(vkGetQueryPoolResults(device_, casterQueries_, 0, 2, sizeof(t), t, sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "Shadow caster timestamps");
        const uint64_t mask = timestampValidBits_ == 64 ? UINT64_MAX : (uint64_t{1} << timestampValidBits_) - 1;
        list.stats.gpuCullMs = double((t[1] - t[0]) & mask) * timestampPeriod_ / 1.e6;
    }
    list.stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    list.active = true;
}
