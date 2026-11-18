#include "RenderPlanning.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>

void validateRenderSettings(const RenderSettings& s)
{
    const auto finite = [](float f) { return std::isfinite(f); };
    if (s.display > RenderDisplay::ShadowMap || !finite(s.shadowBias) || s.shadowBias < 0 || s.shadowBias > .08f ||
        !finite(s.shadowCoverage) || s.shadowCoverage < .05f || s.shadowCoverage > 1 ||
        !finite(s.maxDistance) || s.maxDistance < 0 || !finite(s.lightDirection.x) ||
        !finite(s.lightDirection.y) || !finite(s.lightDirection.z) || glm::length(s.lightDirection) < 1e-4f ||
        !finite(glm::length(s.lightDirection))) throw std::runtime_error("Invalid rendering settings.");
}
bool clusterVisible(const Cluster& c, float padding, const glm::mat4& vp, glm::vec3 eye, const RenderSettings& s)
{
    const auto lo = c.boundsMin - glm::vec3(padding), hi = c.boundsMax + glm::vec3(padding);
    if (s.maxDistance > 0 && glm::length((lo + hi) * .5f - eye) > s.maxDistance + glm::length(hi - lo) * .5f)
        return false;
    if (!s.frustumPreview) return true;
    // Vulkan clip depth is [0,w]. Reject only if every corner is outside one plane.
    bool outside[6] = {true, true, true, true, true, true};
    for (int corner = 0; corner < 8; ++corner) {
        const auto p = vp * glm::vec4((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                                    (corner & 4) ? hi.z : lo.z, 1);
        const bool tests[] = {p.x < -p.w, p.x > p.w, p.y < -p.w, p.y > p.w, p.z < 0, p.z > p.w};
        for (int plane = 0; plane < 6; ++plane) outside[plane] = outside[plane] && tests[plane];
    }
    return std::none_of(std::begin(outside), std::end(outside), [](bool b) { return b; });
}
glm::mat4 shadowViewProjection(const std::vector<Cluster>& clusters, float padding, glm::vec3 direction, float coverage)
{
    if (clusters.empty() || !std::isfinite(padding) || padding < 0) throw std::runtime_error("Invalid shadow bounds.");
    RenderSettings s; s.lightDirection = direction; s.shadowCoverage = coverage; validateRenderSettings(s);
    glm::vec3 lo = clusters.front().boundsMin, hi = clusters.front().boundsMax;
    for (const auto& c : clusters) { lo = glm::min(lo, c.boundsMin); hi = glm::max(hi, c.boundsMax); }
    lo -= glm::vec3(padding); hi += glm::vec3(padding);
    const auto center = (lo + hi) * .5f;
    const float radius = std::max(.01f, glm::length(hi - lo) * .5f);
    direction = glm::normalize(direction);
    const auto up = std::abs(direction.y) > .95f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const auto view = glm::lookAtRH(center + direction * (3 * radius), center, up);
    glm::vec3 lightLo(std::numeric_limits<float>::max()), lightHi(std::numeric_limits<float>::lowest());
    for (int corner = 0; corner < 8; ++corner) {
        const auto p = glm::vec3(view * glm::vec4((corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y,
                                               (corner & 4) ? hi.z : lo.z, 1));
        lightLo = glm::min(lightLo, p); lightHi = glm::max(lightHi, p);
    }
    const float margin = std::max(.001f, radius * .05f);
    // Center the footprint on the scene in light space. Keep the entire fitted depth interval
    // so casters closer to the light can still project onto receivers inside the footprint.
    const glm::vec2 inset = (1.f - coverage) * .5f * (glm::vec2(lightHi - lightLo) + 2.f * margin);
    lightLo.x += inset.x; lightLo.y += inset.y;
    lightHi.x -= inset.x; lightHi.y -= inset.y;
    auto projection = glm::orthoRH_ZO(lightLo.x - margin, lightHi.x + margin, lightLo.y - margin, lightHi.y + margin,
                                     std::max(.001f, -lightHi.z - margin), -lightLo.z + margin);
    projection[1][1] *= -1; // same positive-height Vulkan viewport in both passes
    return projection * view;
}
