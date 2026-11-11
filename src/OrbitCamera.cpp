#include "OrbitCamera.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace {

constexpr glm::vec3 kInitialDirection{3.0f, 2.25f, 4.0f};
constexpr float kOrbitSpeed = 0.006f;
constexpr float kZoomSpeed = 0.12f;
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
    distance_ = std::clamp(distance_, radius_ * 1.15f, radius_ * 12.0f);
}

void OrbitCamera::reset()
{
    distance_ = 1.2f * radius_ / std::sin(kVerticalFov * 0.5f);
    yaw_ = std::atan2(kInitialDirection.x, kInitialDirection.z);
    pitch_ = std::asin(kInitialDirection.y / glm::length(kInitialDirection));
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
    const float nearPlane = std::max(0.01f * radius_, distance - 1.5f * radius_);
    const float farPlane = distance + 1.5f * radius_;
    glm::mat4 projection = glm::perspectiveRH_ZO(kVerticalFov, aspectRatio, nearPlane, farPlane);
    projection[1][1] *= -1.0f;
    return projection * view;
}
