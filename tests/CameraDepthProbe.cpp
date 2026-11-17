#include "VulkanRenderer.h"
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
void checked(VkResult value, const char* op)
{
    if (value != VK_SUCCESS) throw std::runtime_error(std::string("Camera depth probe: ") + op);
}
struct Probe {
    VkDevice device;
    VkDescriptorPool pool{};
    VkDescriptorSetLayout bindings{};
    VkPipelineLayout layout{};
    VkShaderModule shader{};
    VkPipeline pipeline{};
    ~Probe()
    {
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, shader, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyDescriptorSetLayout(device, bindings, nullptr);
    }
};
constexpr uint32_t code[] = {
#include "probe_camera_depth.comp.inc"
};
} // namespace

void VulkanRenderer::checkCameraDepthSampling()
{
    const auto source = cameraDepth();
    if (!source.view) throw std::runtime_error("No submitted camera depth to sample.");
    checked(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX), "wait for frame");
    Probe p{device_};
    const VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo bi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bi.bindingCount = 2;
    bi.pBindings = bindings;
    checked(vkCreateDescriptorSetLayout(device_, &bi, nullptr, &p.bindings), "descriptor layout");
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &p.bindings;
    checked(vkCreatePipelineLayout(device_, &li, nullptr, &p.layout), "pipeline layout");
    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = sizeof(code);
    si.pCode = code;
    checked(vkCreateShaderModule(device_, &si, nullptr, &p.shader), "shader");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.layout = p.layout;
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = p.shader;
    ci.stage.pName = "main";
    checked(vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &p.pipeline), "pipeline");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    checked(vkCreateDescriptorPool(device_, &pi, nullptr, &p.pool), "pool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = p.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.bindings;
    VkDescriptorSet set{};
    checked(vkAllocateDescriptorSets(device_, &ai, &set), "set");
    Buffer output;
    std::array<float, 25> samples{};
    try {
        createBuffer(output, sizeof(samples), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const VkDescriptorImageInfo image{source.sampler, source.view, source.layout};
        const VkDescriptorBufferInfo buffer{output.handle, 0, sizeof(samples)};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = sizes[i].type;
        }
        writes[0].pImageInfo = &image;
        writes[1].pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
        checked(vkResetCommandBuffer(commandBuffer_, 0), "reset");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(commandBuffer_, &begin), "begin");
        vkCmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        vkCmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(commandBuffer_, 1, 1, 1);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &host, 0, nullptr, 0, nullptr);
        checked(vkEndCommandBuffer(commandBuffer_), "end");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer_;
        checked(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "submit");
        checked(vkQueueWaitIdle(graphicsQueue_), "wait");
        void* mapped = nullptr;
        checked(vkMapMemory(device_, output.memory, 0, sizeof(samples), 0, &mapped), "map");
        std::memcpy(samples.data(), mapped, sizeof(samples));
        vkUnmapMemory(device_, output.memory);
    }
    catch (...) {
        destroyBuffer(output);
        throw;
    }
    destroyBuffer(output);
    const auto pixels = readCameraDepth();
    for (uint32_t i = 0; i < samples.size(); ++i) {
        const auto x = (2 * (i % 5) + 1) * source.extent.width / 10,
                   y = (2 * (i / 5) + 1) * source.extent.height / 10;
        if (samples[i] != pixels[size_t(y) * source.extent.width + x])
            throw std::runtime_error("Sampled camera depth differs from stored pixels.");
    }
}
