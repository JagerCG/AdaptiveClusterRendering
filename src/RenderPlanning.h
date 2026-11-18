#pragma once
#include "ClusterBuilder.h"
#include <glm/mat4x4.hpp>

enum class RenderDisplay : uint32_t { Shaded, Depth, ShadowMap };
struct RenderSettings {
    RenderDisplay display = RenderDisplay::Shaded;
    bool shadows = false;
    bool shadowCulling = false;
    float shadowCoverage = 1.0f; // fraction of the fitted light width/height, independent of the camera
    float shadowBias = 0.002f; // normalized light-depth comparison bias
    glm::vec3 lightDirection{0.6f, 0.8f, 1.0f}; // surface toward directional light
    bool frustumPreview = false; // Optional main-pass CPU draw preview
    float maxDistance = 0; // model units; zero disables the CPU distance filter
};
void validateRenderSettings(const RenderSettings& settings);
bool clusterVisible(const Cluster& cluster, float padding, const glm::mat4& viewProjection,
                    glm::vec3 eye, const RenderSettings& settings);
// Camera-independent fit over source bounds plus conservative displacement.
glm::mat4 shadowViewProjection(const std::vector<Cluster>& clusters, float padding, glm::vec3 lightDirection,
                               float coverage = 1.0f);
