#include "GpuLodSelection.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr uint32_t kNoParent = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kSelectionShader[] = {
#include "lod_select.comp.inc"
};

struct SelectionParams {
    glm::vec4 eyeNear;
    glm::vec4 forwardScale;
    glm::vec4 centerRadius;
    glm::uvec4 options;
    glm::vec4 thresholds;
};

static_assert(sizeof(GpuLodNode) == 48);
static_assert(sizeof(GpuCluster) == 32);
static_assert(sizeof(SelectionParams) == 80);
static_assert(sizeof(LodStats) == 40);
static_assert(sizeof(VkDrawIndexedIndirectCommand) == 20);

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed (VkResult " + std::to_string(result) + ")");
}

}

PackedLodScene packLodScene(const LodHierarchy& hierarchy)
{
    if (hierarchy.levels.empty() || hierarchy.levels.size() > 8 || hierarchy.nodes.empty())
        throw std::invalid_argument("GPU LOD requires one to eight nonempty levels.");
    PackedLodScene scene;
    scene.levelCount = uint32_t(hierarchy.levels.size());
    std::vector<uint32_t> clusterOffsets;
    for (uint32_t levelId = 0; levelId < hierarchy.levels.size(); ++levelId) {
        const auto& level = hierarchy.levels[levelId];
        const auto limit = std::numeric_limits<uint32_t>::max();
        if (level.mesh.vertices.size() > limit - scene.mesh.vertices.size() ||
            level.dataset.indices.size() > limit - scene.mesh.indices.size() ||
            level.dataset.clusters.size() > limit - scene.clusters.size())
            throw std::overflow_error("Packed LOD scene exceeds 32-bit limits.");
        const auto vertexOffset = uint32_t(scene.mesh.vertices.size());
        const auto indexOffset = uint32_t(scene.mesh.indices.size());
        clusterOffsets.push_back(uint32_t(scene.clusters.size()));
        scene.mesh.vertices.insert(scene.mesh.vertices.end(), level.mesh.vertices.begin(), level.mesh.vertices.end());
        for (uint32_t index : level.dataset.indices) {
            if (index >= level.mesh.vertices.size()) throw std::invalid_argument("Invalid LOD vertex index.");
            scene.mesh.indices.push_back(vertexOffset + index);
        }
        for (uint32_t id = 0; id < level.dataset.clusters.size(); ++id) {
            const auto& cluster = level.dataset.clusters[id];
            if (cluster.firstIndex > level.dataset.indices.size() || cluster.indexCount % 3 != 0 ||
                cluster.indexCount > level.dataset.indices.size() - cluster.firstIndex)
                throw std::invalid_argument("Invalid LOD cluster range.");
            scene.clusters.push_back({{indexOffset + cluster.firstIndex, cluster.indexCount, kNoParent, levelId},
                                      {id, cluster.groupId, 0, 0}});
        }
    }
    scene.nodes.resize(hierarchy.nodes.size());
    for (uint32_t id = 0; id < hierarchy.nodes.size(); ++id) {
        const auto& node = hierarchy.nodes[id];
        if (node.level >= hierarchy.levels.size() || !std::isfinite(node.error) || node.error < 0)
            throw std::invalid_argument("Invalid LOD node.");
        const auto clusterCount = hierarchy.levels[node.level].dataset.clusters.size();
        if (node.firstCluster > clusterCount || node.clusterCount == 0 ||
            node.clusterCount > clusterCount - node.firstCluster)
            throw std::invalid_argument("Invalid LOD node geometry range.");
        const uint32_t first = clusterOffsets[node.level] + node.firstCluster;
        scene.nodes[id] = {glm::vec4(node.boundsMin, node.error), glm::vec4(node.boundsMax, 0),
                           {kNoParent, first, node.clusterCount, node.level}};
        for (uint32_t c = 0; c < node.clusterCount; ++c) {
            auto& owner = scene.clusters[first + c].geometry.z;
            if (owner != kNoParent) throw std::invalid_argument("A cluster belongs to multiple LOD nodes.");
            owner = id;
        }
    }
    for (uint32_t id = 0; id < hierarchy.nodes.size(); ++id) {
        for (uint32_t child : hierarchy.nodes[id].children) {
            if (child >= id || hierarchy.nodes[child].level + 1 != hierarchy.nodes[id].level ||
                scene.nodes[child].links.x != kNoParent)
                throw std::invalid_argument("Invalid LOD parent relation.");
            scene.nodes[child].links.x = id;
        }
    }
    for (const auto& cluster : scene.clusters)
        if (cluster.geometry.z == kNoParent) throw std::invalid_argument("A cluster has no LOD node.");
    if (scene.mesh.vertices.empty() || scene.clusters.empty()) throw std::invalid_argument("Empty LOD geometry.");
    glm::vec3 minimum = hierarchy.levels.front().mesh.vertices.front().position;
    glm::vec3 maximum = minimum;
    for (const auto& vertex : hierarchy.levels.front().mesh.vertices) {
        minimum = glm::min(minimum, vertex.position);
        maximum = glm::max(maximum, vertex.position);
    }
    scene.centerRadius = glm::vec4((minimum + maximum) * 0.5f, glm::length(maximum - minimum) * 0.5f);
    return scene;
}

GpuLodSelection::~GpuLodSelection()
{
    if (device_ == VK_NULL_HANDLE) return;
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
    vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
    for (auto& buffer : buffers_) destroyBuffer(buffer);
}

void GpuLodSelection::initialize(VkPhysicalDevice gpu, VkDevice device)
{
    gpu_ = gpu;
    device_ = device;
    VkDescriptorSetLayoutBinding bindings[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    bindings[1].stageFlags |= VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout.bindingCount = 4;
    layout.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &layout, nullptr, &descriptorLayout_), "vkCreateDescriptorSetLayout");

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &poolSize;
    check(vkCreateDescriptorPool(device_, &pool, nullptr, &descriptorPool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descriptorPool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptorLayout_;
    check(vkAllocateDescriptorSets(device_, &allocate, &descriptorSet_), "vkAllocateDescriptorSets");

    const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SelectionParams)};
    VkPipelineLayoutCreateInfo pipelineLayout{};
    pipelineLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &descriptorLayout_;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &range;
    check(vkCreatePipelineLayout(device_, &pipelineLayout, nullptr, &pipelineLayout_), "vkCreatePipelineLayout");
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kSelectionShader);
    shaderInfo.pCode = kSelectionShader;
    VkShaderModule shader = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &shaderInfo, nullptr, &shader), "vkCreateShaderModule");
    VkComputePipelineCreateInfo pipeline{};
    pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = shader;
    pipeline.stage.pName = "main";
    pipeline.layout = pipelineLayout_;
    const auto result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, shader, nullptr);
    check(result, "vkCreateComputePipelines");
}

GpuLodSelection::Buffer GpuLodSelection::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage)
{
    Buffer buffer;
    try {
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device_, &info, nullptr, &buffer.handle), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(gpu_, &memory);
        const auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = memory.memoryTypeCount;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                type = i;
                break;
            }
        }
        if (type == memory.memoryTypeCount) throw std::runtime_error("No host-visible coherent memory for LOD data.");
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
        check(vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped), "vkMapMemory");
        return buffer;
    } catch (...) {
        destroyBuffer(buffer);
        throw;
    }
}

void GpuLodSelection::destroyBuffer(Buffer& buffer)
{
    if (buffer.mapped) vkUnmapMemory(device_, buffer.memory);
    vkDestroyBuffer(device_, buffer.handle, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
}

void GpuLodSelection::upload(const PackedLodScene& scene)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_, &properties);
    const VkDeviceSize sizes[] = {scene.nodes.size() * sizeof(GpuLodNode), scene.clusters.size() * sizeof(GpuCluster),
                                 scene.clusters.size() * sizeof(VkDrawIndexedIndirectCommand), sizeof(LodStats)};
    for (auto size : sizes)
        if (size == 0 || size > properties.limits.maxStorageBufferRange)
            throw std::runtime_error("LOD data exceeds the device's storage buffer range.");
    if ((scene.clusters.size() + 63) / 64 > properties.limits.maxComputeWorkGroupCount[0])
        throw std::runtime_error("LOD dispatch exceeds the device's group-count limit.");
    std::array<Buffer, 4> replacement{};
    try {
        replacement[0] = createBuffer(sizes[0], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        replacement[1] = createBuffer(sizes[1], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        replacement[2] = createBuffer(sizes[2], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        replacement[3] = createBuffer(sizes[3], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        std::memcpy(replacement[0].mapped, scene.nodes.data(), size_t(sizes[0]));
        std::memcpy(replacement[1].mapped, scene.clusters.data(), size_t(sizes[1]));
        std::memset(replacement[3].mapped, 0, sizeof(LodStats));
    } catch (...) {
        for (auto& buffer : replacement) destroyBuffer(buffer);
        throw;
    }
    VkDescriptorBufferInfo infos[4]{};
    VkWriteDescriptorSet writes[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        infos[i] = {replacement[i].handle, 0, sizes[i]};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    for (auto& buffer : buffers_) destroyBuffer(buffer);
    buffers_ = replacement;
    centerRadius_ = scene.centerRadius;
    clusterCount_ = uint32_t(scene.clusters.size());
    levelCount_ = scene.levelCount;
    stats_ = {};
}

void GpuLodSelection::record(VkCommandBuffer commands, const CameraView& camera, const LodSettings& settings)
{
    if (!std::isfinite(settings.pixelError) || settings.pixelError <= 0)
        throw std::invalid_argument("Pixel error must be positive and finite.");
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(commands, buffers_[3].handle, 0, sizeof(LodStats), 0);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);

    SelectionParams params{glm::vec4(camera.eye, camera.nearPlane), glm::vec4(camera.forward, camera.pixelScale),
                           centerRadius_, {uint32_t(settings.mode), std::min(settings.level, levelCount_ - 1),
                                           clusterCount_, levelCount_}, glm::vec4(settings.pixelError, 0, 0, 0)};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(commands, pipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(commands, (clusterCount_ + 63) / 64, 1, 1);

    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void GpuLodSelection::readStats()
{
    if (buffers_[3].mapped) std::memcpy(&stats_, buffers_[3].mapped, sizeof(stats_));
}
