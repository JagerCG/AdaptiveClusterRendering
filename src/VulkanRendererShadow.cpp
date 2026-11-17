#include "VulkanRenderer.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
void checked(VkResult r, const char* operation) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(r));
}
constexpr VkImageSubresourceRange depthRange{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
}

void VulkanRenderer::setRenderSettings(const RenderSettings& settings)
{
    validateRenderSettings(settings);
    if (settings.culling != renderSettings_.culling) {
        visibilityActive_ = false; hiZMips_.clear(); visStats_ = {};
    }
    renderSettings_ = settings; // display/camera/light changes never invalidate generated geometry
}

void VulkanRenderer::createShadowResources()
{
    VkFormatProperties format{};
    vkGetPhysicalDeviceFormatProperties(gpu_, VK_FORMAT_D32_SFLOAT, &format);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & required) != required)
        throw std::runtime_error("D32 sampled/transfer shadow depth is unsupported on this GPU.");
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT; depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0; dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1; pass.pAttachments = &depth; pass.subpassCount = 1; pass.pSubpasses = &subpass;
    pass.dependencyCount = 2; pass.pDependencies = dependencies;
    checked(vkCreateRenderPass(device_, &pass, nullptr, &shadowRenderPass_), "Create shadow pass");
    VkImageCreateInfo image{};
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_D32_SFLOAT; image.extent = {shadowMapSize, shadowMapSize, 1};
    image.mipLevels = image.arrayLayers = 1; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checked(vkCreateImage(device_, &image, nullptr, &shadowImage_), "Create shadow image");
    VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device_, shadowImage_, &requirements);
    shadowMemory_ = allocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checked(vkBindImageMemory(device_, shadowImage_, shadowMemory_, 0), "Bind shadow image");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; view.image = shadowImage_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format; view.subresourceRange = depthRange;
    checked(vkCreateImageView(device_, &view, nullptr, &shadowView_), "Create shadow view");
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    checked(vkCreateSampler(device_, &sampler, nullptr, &shadowSampler_), "Create shadow sampler");
    VkFramebufferCreateInfo framebuffer{};
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; framebuffer.renderPass = shadowRenderPass_;
    framebuffer.attachmentCount = 1; framebuffer.pAttachments = &shadowView_;
    framebuffer.width = framebuffer.height = shadowMapSize; framebuffer.layers = 1;
    checked(vkCreateFramebuffer(device_, &framebuffer, nullptr, &shadowFramebuffer_), "Create shadow framebuffer");
    const VkDescriptorImageInfo imageInfo{shadowSampler_, shadowView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorBufferInfo bufferInfo{lightingBuffer_.handle, 0, 80};
    VkWriteDescriptorSet writes[2]{};
    for (auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = shadowSet_; write.descriptorCount = 1; }
    writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &imageInfo;
    writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[1].pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    // A valid, lit fallback must exist even before the first shadow-enabled frame.
    checked(vkResetCommandBuffer(commandBuffer_, 0), "Reset shadow initialization");
    VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vkBeginCommandBuffer(commandBuffer_, &begin), "Begin shadow initialization");
    VkImageMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = shadowImage_; barrier.subresourceRange = depthRange;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    const VkClearDepthStencilValue clear{1, 0};
    vkCmdClearDepthStencilImage(commandBuffer_, shadowImage_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &depthRange);
    barrier.oldLayout = barrier.newLayout; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    checked(vkEndCommandBuffer(commandBuffer_), "End shadow initialization");
    VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer_;
    checked(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Submit shadow initialization");
    checked(vkQueueWaitIdle(graphicsQueue_), "Wait shadow initialization");
}

void VulkanRenderer::destroyShadowResources()
{
    vkDestroyFramebuffer(device_, shadowFramebuffer_, nullptr);
    vkDestroySampler(device_, shadowSampler_, nullptr);
    vkDestroyImageView(device_, shadowView_, nullptr);
    vkDestroyImage(device_, shadowImage_, nullptr);
    vkFreeMemory(device_, shadowMemory_, nullptr);
}

std::vector<float> VulkanRenderer::readShadowDepth()
{
    return readDepth(shadowImage_, {shadowMapSize,shadowMapSize}, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

std::vector<float> VulkanRenderer::readCameraDepth()
{
    if (!cameraDepth_.view) throw std::runtime_error("No main-camera depth is available for this frame.");
    return readDepth(depthImage_, cameraDepth_.extent, cameraDepth_.layout);
}

std::vector<float> VulkanRenderer::readDepth(VkImage image, VkExtent2D extent, VkImageLayout layout)
{
    checked(vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX), "Wait depth readback");
    std::vector<float> pixels(size_t(extent.width) * extent.height);
    Buffer staging;
    createBuffer(staging, pixels.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    try {
        checked(vkResetCommandBuffer(commandBuffer_, 0), "Reset shadow readback");
        VkCommandBufferBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(commandBuffer_, &begin), "Begin shadow readback");
        VkImageMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = image; barrier.subresourceRange = depthRange;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout = layout; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(commandBuffer_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1, &copy);
        barrier.oldLayout = barrier.newLayout; barrier.newLayout = layout;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferMemoryBarrier host{}; host.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = staging.handle; host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        checked(vkEndCommandBuffer(commandBuffer_), "End shadow readback");
        VkSubmitInfo submit{}; submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; submit.commandBufferCount = 1; submit.pCommandBuffers = &commandBuffer_;
        checked(vkQueueSubmit(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "Submit shadow readback");
        checked(vkQueueWaitIdle(graphicsQueue_), "Wait shadow copy");
        void* mapped = nullptr; checked(vkMapMemory(device_, staging.memory, 0, pixels.size() * sizeof(float), 0, &mapped), "Map shadow depth");
        std::memcpy(pixels.data(), mapped, pixels.size() * sizeof(float)); vkUnmapMemory(device_, staging.memory);
    } catch (...) { destroyBuffer(staging); throw; }
    destroyBuffer(staging);
    return pixels;
}
