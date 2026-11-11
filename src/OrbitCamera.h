#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class OrbitCamera {
public:
    OrbitCamera(glm::vec3 target, float radius);

    void orbit(float deltaX, float deltaY);
    void zoom(float scrollSteps);
    void reset();
    glm::mat4 viewProjection(float aspectRatio) const;

private:
    glm::vec3 target_;
    float radius_;
    float distance_;
    float yaw_;
    float pitch_;
};
