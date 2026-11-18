#include "ComputeGeometry.h"

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
constexpr uint32_t kGenerationShader[] = {
#include "generate_geometry.comp.inc"
};
struct GenerationParams {
    uint32_t vertexWordOffset, indexWordOffset, sourceTriangleCount, edgeSegments;
    float amplitude, maxDisplacement;
};
static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, normal) == 12 && offsetof(Vertex, texcoord) == 24);
static_assert(sizeof(GenerationParams) == 24);

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) +
        " failed (VkResult " + std::to_string(result) + ")");
}
} // namespace

ComputeGeometry::~ComputeGeometry()
{
    if (!device_) return; // caller has waited for all users of these resources
    destroyBuffer(vertices_);
    destroyBuffer(indices_);
    destroyBuffer(work_);
    destroyHeight(height_);
    vkDestroyQueryPool(device_, queries_, nullptr);
    vkDestroyPipeline(device_, pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, layout_, nullptr);
    vkDestroyDescriptorPool(device_, pool_, nullptr);
    vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
}

void ComputeGeometry::initialize(VkPhysicalDevice gpu, VkDevice device, uint32_t queueFamily)
{
    if (device_) throw std::runtime_error("Compute geometry is already initialized.");
    gpu_ = gpu;
    device_ = device;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_, &properties);
    limits_.maxStorageBufferBytes = properties.limits.maxStorageBufferRange;
    limits_.maxWorkgroupsX = properties.limits.maxComputeWorkGroupCount[0];
    timestampPeriod_ = properties.limits.timestampPeriod;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu_, &count, families.data());
    if (queueFamily >= count || !(families[queueFamily].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        throw std::runtime_error("Compute geometry requires a compute-capable queue.");
    }
    timestampBits_ = families[queueFamily].timestampValidBits;
    if (timestampBits_ && timestampPeriod_ > 0) {
        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2;
        check(vkCreateQueryPool(device_, &info, nullptr, &queries_), "vkCreateQueryPool (compute)");
    }
    VkDescriptorSetLayoutBinding bindings[6]{};
    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 6;
    setInfo.pBindings = bindings;
    check(vkCreateDescriptorSetLayout(device_, &setInfo, nullptr, &setLayout_), "vkCreateDescriptorSetLayout (compute)");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &pool_), "vkCreateDescriptorPool (compute)");
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = pool_;
    allocation.descriptorSetCount = 1;
    allocation.pSetLayouts = &setLayout_;
    check(vkAllocateDescriptorSets(device_, &allocation, &set_), "vkAllocateDescriptorSets (compute)");
    const VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(GenerationParams)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &constants;
    check(vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_), "vkCreatePipelineLayout (compute)");
    VkShaderModuleCreateInfo shaderInfo{};
    shaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shaderInfo.codeSize = sizeof(kGenerationShader);
    shaderInfo.pCode = kGenerationShader;
    VkShaderModule shader = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_, &shaderInfo, nullptr, &shader), "vkCreateShaderModule (compute)");
    VkComputePipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = shader;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = layout_;
    const VkResult result = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_);
    vkDestroyShaderModule(device_, shader, nullptr);
    check(result, "vkCreateComputePipelines");
}

void ComputeGeometry::createBuffer(Buffer& buffer, VkDeviceSize bytes, VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags properties)
{
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_, &info, nullptr, &buffer.handle), "vkCreateBuffer (compute)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device_, buffer.handle, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(gpu_, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (1u << i)) ||
            (memory.memoryTypes[i].propertyFlags & properties) != properties) continue;
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = i;
        check(vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory), "vkAllocateMemory (compute)");
        buffer.allocationBytes = requirements.size;
        check(vkBindBufferMemory(device_, buffer.handle, buffer.memory, 0), "vkBindBufferMemory (compute)");
        return;
    }
    throw std::runtime_error("No compatible compute buffer memory type.");
}

void ComputeGeometry::destroyBuffer(Buffer& buffer)
{
    vkDestroyBuffer(device_, buffer.handle, nullptr);
    vkFreeMemory(device_, buffer.memory, nullptr);
    buffer = {};
}

void ComputeGeometry::destroyHeight(HeightImage& height)
{
    vkDestroySampler(device_, height.sampler, nullptr);
    vkDestroyImageView(device_, height.view, nullptr);
    vkDestroyImage(device_, height.image, nullptr);
    vkFreeMemory(device_, height.memory, nullptr);
    height = {};
}

void ComputeGeometry::uploadHeight(const HeightField& field, VkCommandBuffer command, VkQueue queue)
{
    const float unitBound = displacementBound(field, {true, 1.f}); // Validate and scan once per uploaded height image.
    if (heightRevision_ == std::numeric_limits<uint64_t>::max()) throw std::runtime_error("Height revision overflow.");
    auto nextField = field;
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(gpu_, &properties);
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(gpu_, VK_FORMAT_R32_SFLOAT, &format);
    const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & required) != required ||
        field.width > properties.limits.maxImageDimension2D || field.height > properties.limits.maxImageDimension2D)
        throw std::runtime_error("Device does not support this R32_SFLOAT height image.");
    HeightImage next;
    Buffer staging;
    const VkDeviceSize bytes = field.samples.size() * sizeof(float);
    try {
        VkImageCreateInfo image{};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R32_SFLOAT;
        image.extent = {field.width, field.height, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device_, &image, nullptr, &next.image), "vkCreateImage (height)");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, next.image, &requirements);
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(gpu_, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if (!(requirements.memoryTypeBits & (1u << i)) ||
                !(memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            VkMemoryAllocateInfo allocation{};
            allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = i;
            check(vkAllocateMemory(device_, &allocation, nullptr, &next.memory), "vkAllocateMemory (height)");
            next.allocationBytes = requirements.size;
            break;
        }
        if (!next.memory) throw std::runtime_error("No compatible height image memory type.");
        check(vkBindImageMemory(device_, next.image, next.memory, 0), "vkBindImageMemory (height)");
        VkImageViewCreateInfo view{};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = next.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R32_SFLOAT;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_, &view, nullptr, &next.view), "vkCreateImageView (height)");
        VkSamplerCreateInfo sampler{};
        sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(device_, &sampler, nullptr, &next.sampler), "vkCreateSampler (height)");
        createBuffer(staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* mapped = nullptr;
        check(vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped), "vkMapMemory (height)");
        std::memcpy(mapped, field.samples.data(), static_cast<size_t>(bytes));
        vkUnmapMemory(device_, staging.memory);
        check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer (height)");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer (height)");
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = next.image;
        barrier.subresourceRange = view.subresourceRange;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = image.extent;
        vkCmdCopyBufferToImage(command, staging.handle, next.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.oldLayout = barrier.newLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer (height)");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit (height)");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle (height)");
        VkDescriptorImageInfo descriptor{next.sampler, next.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set_; write.dstBinding = 5; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descriptor;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    } catch (...) {
        destroyBuffer(staging);
        destroyHeight(next);
        throw;
    }
    destroyBuffer(staging);
    destroyHeight(height_);
    height_ = next;
    heightField_ = std::move(nextField);
    heightUnitBound_ = unitBound;
    ++heightRevision_;
    invalidate();
}

bool ComputeGeometry::matches(const SceneGpuData& source, const ResolvedClusterSelection& selection, uint32_t edgeSegments,
                              const DisplacementSettings& displacement) const
{
    return output_.valid && output_.heightRevision == heightRevision_ && sameDisplacement(output_.displacement, displacement) &&
        output_.edgeSegments == edgeSegments && source_.sceneRevision == source.sceneRevision &&
        source_.vertexBuffer == source.vertexBuffer && source_.indexBuffer == source.indexBuffer &&
        source_.vertexOffset == source.vertexOffset && source_.indexOffset == source.indexOffset &&
        source_.vertexCount == source.vertexCount && source_.indexCount == source.indexCount &&
        source_.clusterCount == source.clusterCount && source_.vertexStride == source.vertexStride &&
        source_.indexStride == source.indexStride && sameClusterSelection(selection_, selection);
}

void ComputeGeometry::prepare(const SceneGpuData& source, const ResolvedClusterSelection& selection, uint32_t edgeSegments,
                              const DisplacementSettings& displacement)
{
    validateDisplacement(displacement);
    started_ = std::chrono::steady_clock::now();
    invalidate();
    pending_ = false;
    if (!pipeline_ || !height_.image || source.sceneRevision != selection.sceneRevision ||
        source.vertexStride != 32 || source.indexStride != 4 ||
        !source.vertexBuffer || !source.indexBuffer || source.vertexCount == 0 ||
        source.vertexOffset % 4 || source.indexOffset % 4 ||
        generationCounter_ == std::numeric_limits<uint64_t>::max()) {
        throw std::runtime_error("Invalid source ABI / revision for compute geometry.");
    }
    // Bind at zero and pass word offsets, since the combined source IB offset
    // need not satisfy minStorageBufferOffsetAlignment. Check before addition.
    const uint64_t vertexBytes = uint64_t(source.vertexCount) * 32;
    const uint64_t indexBytes = uint64_t(source.indexCount) * 4;
    if (vertexBytes > limits_.maxStorageBufferBytes || indexBytes > limits_.maxStorageBufferBytes ||
        source.vertexOffset > limits_.maxStorageBufferBytes - vertexBytes ||
        source.indexOffset > limits_.maxStorageBufferBytes - indexBytes) {
        throw std::runtime_error("Source descriptor exceeds maxStorageBufferRange.");
    }
    auto plan = makeGeometryGenerationPlan(source.indexCount, source.clusterCount, selection, edgeSegments, limits_,
                                           source.triangleWork == VK_NULL_HANDLE);
    auto nextSelection = selection;
    GeneratedGeometry next;
    next.sourceRevision = source.sceneRevision;
    next.heightRevision = heightRevision_;
    next.displacement = displacement;
    next.maxDisplacement = maxDisplacement(displacement);
    next.vertexCount = plan.vertexCount;
    next.indexCount = plan.indexCount;
    next.edgeSegments = plan.edgeSegments;
    next.sourceTriangleCount = plan.sourceTriangleCount;
    next.vertexBytes = plan.vertexBytes;
    next.indexBytes = plan.indexBytes;
    next.worklistBytes = plan.worklistBytes;
    next.workgroups = plan.workgroupsX;
    next.ranges = std::move(plan.ranges);
    Buffer vertices, indices, work;
    try {
        if (plan.indexCount) {
            const auto outputUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            createBuffer(vertices, plan.vertexBytes, outputUsage | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            createBuffer(indices, plan.indexBytes, outputUsage | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            next.allocatedOutputBytes = vertices.allocationBytes + indices.allocationBytes;
            if (next.allocatedOutputBytes > limits_.maxOutputBytes) {
                throw std::runtime_error("Aligned compute output allocation exceeds the 128 MiB budget.");
            }
            if (source.triangleWork) {
                next.allocatedWorklistBytes = source.triangleWorkAllocation;
            } else {
                createBuffer(work, plan.worklistBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                next.allocatedWorklistBytes = work.allocationBytes;
                void* mapped = nullptr;
                check(vkMapMemory(device_, work.memory, 0, plan.worklistBytes, 0, &mapped), "vkMapMemory (compute work)");
                std::memcpy(mapped, plan.triangleFirstIndices.data(), static_cast<size_t>(plan.worklistBytes));
                vkUnmapMemory(device_, work.memory);
            }
            const VkDescriptorBufferInfo buffers[] = {
                {source.vertexBuffer, 0, source.vertexOffset + vertexBytes},
                {source.indexBuffer, 0, source.indexOffset + indexBytes},
                {source.triangleWork ? source.triangleWork : work.handle, 0, plan.worklistBytes},
                {vertices.handle, 0, plan.vertexBytes},
                {indices.handle, 0, plan.indexBytes}
            };
            VkWriteDescriptorSet writes[5]{};
            for (uint32_t i = 0; i < 5; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set_;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &buffers[i];
            }
            vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
        }
    } catch (...) {
        destroyBuffer(vertices);
        destroyBuffer(indices);
        destroyBuffer(work);
        throw;
    }
    destroyBuffer(vertices_);
    destroyBuffer(indices_);
    destroyBuffer(work_);
    vertices_ = vertices;
    indices_ = indices;
    work_ = work;
    next.vertexBuffer = vertices_.handle;
    next.indexBuffer = indices_.handle;
    output_ = std::move(next);
    source_ = source;
    selection_ = std::move(nextSelection);
    pending_ = true;
}

void ComputeGeometry::record(VkCommandBuffer command)
{
    if (!pending_) throw std::runtime_error("Compute geometry was not prepared.");
    if (!output_.indexCount) return; // no zero-sized buffers, descriptors or dispatch
    VkMemoryBarrier workReady{};
    workReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    workReady.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    workReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &workReady, 0, nullptr, 0, nullptr);
    if (queries_) {
        vkCmdResetQueryPool(command, queries_, 0, 2);
        vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries_, 0);
    }
    const GenerationParams params{static_cast<uint32_t>(source_.vertexOffset / 4),
        static_cast<uint32_t>(source_.indexOffset / 4), output_.sourceTriangleCount, output_.edgeSegments,
        output_.displacement.amplitude, output_.maxDisplacement};
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    if (source_.indirectCommands) {
        const uint32_t level = output_.edgeSegments == 8 ? 3 : output_.edgeSegments == 4 ? 2 : output_.edgeSegments == 2 ? 1 : 0;
        vkCmdDispatchIndirect(command, source_.indirectCommands, 16 + level * sizeof(VkDispatchIndirectCommand));
    } else {
        vkCmdDispatch(command, output_.workgroups, 1, 1);
    }
    if (queries_) vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries_, 1);
    VkBufferMemoryBarrier barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.size = VK_WHOLE_SIZE;
    }
    barriers[0].buffer = vertices_.handle;
    barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].buffer = indices_.handle;
    barriers[1].dstAccessMask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 2, barriers, 0, nullptr);
}

void ComputeGeometry::finish()
{
    if (!pending_) throw std::runtime_error("Compute geometry has no pending generation.");
    if (queries_ && output_.indexCount) {
        uint64_t timestamps[2]{};
        check(vkGetQueryPoolResults(device_, queries_, 0, 2, sizeof(timestamps), timestamps,
              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults (compute)");
        const uint64_t mask = timestampBits_ == 64 ? ~uint64_t{0} : (uint64_t{1} << timestampBits_) - 1;
        output_.gpuGenerateMs = double((timestamps[1] - timestamps[0]) & mask) * double(timestampPeriod_) / 1'000'000.0;
    }
    output_.cpuGenerateMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started_).count();
    output_.generationRevision = ++generationCounter_;
    output_.valid = true;
    pending_ = false;
}
