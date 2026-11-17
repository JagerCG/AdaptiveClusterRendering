#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <cstdint>

class OrbitCamera {
public:
    OrbitCamera(glm::vec3 target, float radius);

    void orbit(float deltaX, float deltaY);
    void zoom(float scrollSteps);
    void reset();
    glm::vec3 eyePosition(float aspectRatio = 1.0f) const;
    glm::mat4 viewProjection(float aspectRatio) const;
    float nearPlane(float aspectRatio) const;
    static float focalPixels(uint32_t height);
    glm::vec3 pose() const { return {yaw_, pitch_, distance_ / radius_}; }
    void setPose(glm::vec3 pose);

private:
    glm::vec3 target_;
    float radius_;
    float distance_;
    float yaw_;
    float pitch_;
};
