#pragma once
#include "ClusterBuilder.h"
#include <glm/mat4x4.hpp>

enum class RenderDisplay : uint32_t { Shaded, Depth, ShadowMap };
struct RenderSettings {
    RenderDisplay display = RenderDisplay::Shaded;
    bool shadows = false;
    float shadowBias = 0.002f; // normalized light-depth comparison bias
    glm::vec3 lightDirection{0.6f, 0.8f, 1.0f}; // surface toward directional light
    bool frustumPreview = false; // CPU draw-only preview, NOT GPU LOD selection
    float maxDistance = 0; // model units; zero disables the CPU distance filter
};
void validateRenderSettings(const RenderSettings& settings);
bool clusterVisible(const Cluster& cluster, float padding, const glm::mat4& viewProjection,
                    glm::vec3 eye, const RenderSettings& settings);
// Camera-independent fit over source bounds plus conservative displacement.
glm::mat4 shadowViewProjection(const std::vector<Cluster>& clusters, float padding, glm::vec3 lightDirection);
