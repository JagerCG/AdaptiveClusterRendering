#pragma once
#include "ClusterBuilder.h"
#include <glm/mat4x4.hpp>

enum class RenderDisplay : uint32_t { Shaded, Depth, ShadowMap };
enum class CullMode : uint32_t { Off, Frustum, Hierarchy, HiZ, FlatHiZ };
constexpr bool usesHiZ(CullMode mode) { return mode == CullMode::HiZ || mode == CullMode::FlatHiZ; }
struct RenderSettings {
    RenderDisplay display = RenderDisplay::Shaded;
    bool shadows = false;
    bool shadowCulling = false;
    float shadowCoverage = 1.0f; // fraction of the fitted light width/height, independent of the camera
    float shadowBias = 0.002f; // normalized light-depth comparison bias
    glm::vec3 lightDirection{0.6f, 0.8f, 1.0f}; // surface toward directional light
    bool frustumPreview = false; // CPU draw-only preview, NOT GPU LOD selection
    float maxDistance = 0; // model units; zero disables the CPU distance filter
    bool groupColors = false;
    bool adaptiveLod = false, levelColors = false;
    float lodPixels = 96.0f;
    CullMode culling = CullMode::Off;
    bool showHiZ = false, showRejectedBounds = false;
    uint32_t hiZMip = 0;
};
void validateRenderSettings(const RenderSettings& settings);
glm::vec4 lodLevelColor(uint32_t level);
bool clusterVisible(const Cluster& cluster, float padding, const glm::mat4& viewProjection,
                    glm::vec3 eye, const RenderSettings& settings);
// Camera-independent fit over source bounds plus conservative displacement.
glm::mat4 shadowViewProjection(const std::vector<Cluster>& clusters, float padding, glm::vec3 lightDirection,
                               float coverage = 1.0f);
