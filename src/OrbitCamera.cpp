#include "OrbitCamera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

constexpr glm::vec3 kInitialDirection{3.0f, 2.25f, 4.0f};
constexpr float kOrbitSpeed = 0.006f;
constexpr float kZoomSpeed = 0.12f;
constexpr float kMinDistance = 1.15f;
constexpr float kMaxDistance = 256.f; // multiples of the framed model radius
constexpr float kMaxPitch = glm::radians(85.0f);
constexpr float kVerticalFov = glm::radians(45.0f);

} // namespace

OrbitCamera::OrbitCamera(glm::vec3 target, float radius)
    : target_(target), radius_(radius)
{
    reset();
}

void OrbitCamera::orbit(float deltaX, float deltaY)
{
    yaw_ -= deltaX * kOrbitSpeed;
    pitch_ = std::clamp(pitch_ + deltaY * kOrbitSpeed, -kMaxPitch, kMaxPitch);
}

void OrbitCamera::zoom(float scrollSteps)
{
    distance_ *= std::exp(-scrollSteps * kZoomSpeed);
    distance_ = std::clamp(distance_, radius_ * kMinDistance, radius_ * kMaxDistance);
}

void OrbitCamera::reset()
{
    distance_ = 1.2f * radius_ / std::sin(kVerticalFov * 0.5f);
    yaw_ = std::atan2(kInitialDirection.x, kInitialDirection.z);
    pitch_ = std::asin(kInitialDirection.y / glm::length(kInitialDirection));
}

glm::vec3 OrbitCamera::eyePosition(float aspectRatio) const
{
    const float horizontal = std::cos(pitch_);
    const glm::vec3 direction{
        horizontal * std::sin(yaw_),
        std::sin(pitch_),
        horizontal * std::cos(yaw_)
    };
    return target_ + (distance_ / std::min(aspectRatio, 1.0f)) * direction;
}

float OrbitCamera::nearPlane(float aspect) const
{
    return std::max(.01f * radius_, distance_ / std::min(aspect, 1.0f) - 1.5f * radius_);
}

float OrbitCamera::focalPixels(uint32_t height) { return .5f * height / std::tan(.5f * kVerticalFov); }

void OrbitCamera::setPose(glm::vec3 p)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
        std::abs(p.y) > kMaxPitch + 1e-5f || p.z < kMinDistance - 1e-5f ||
        p.z > kMaxDistance * (1.f + 1e-6f))
        throw std::runtime_error("Invalid saved orbit camera.");
    yaw_ = p.x;
    pitch_ = p.y;
    distance_ = std::clamp(p.z, kMinDistance, kMaxDistance) * radius_;
}

glm::mat4 OrbitCamera::viewProjection(float aspectRatio) const
{
    const float horizontal = std::cos(pitch_);
    const glm::vec3 direction{
        horizontal * std::sin(yaw_),
        std::sin(pitch_),
        horizontal * std::cos(yaw_)
    };
    // In a narrow window the horizontal field of view limits the fit.
    const float distance = distance_ / std::min(aspectRatio, 1.0f);
    const glm::vec3 eye = target_ + distance * direction;
    const glm::mat4 view = glm::lookAtRH(eye, target_, glm::vec3(0.0f, 1.0f, 0.0f));
    const float farPlane = distance + 1.5f * radius_;
    glm::mat4 projection = glm::perspectiveRH_ZO(kVerticalFov, aspectRatio, nearPlane(aspectRatio), farPlane);
    projection[1][1] *= -1.0f;
    return projection * view;
}
