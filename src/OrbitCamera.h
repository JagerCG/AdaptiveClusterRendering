#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

struct CameraView {
    glm::mat4 viewProjection;
    glm::vec3 eye;
    float nearPlane;
    glm::vec3 forward;
    float pixelScale;
};

class OrbitCamera {
public:
    OrbitCamera(glm::vec3 target, float radius);

    void orbit(float deltaX, float deltaY);
    void zoom(float scrollSteps);
    void reset();
    glm::mat4 viewProjection(float aspectRatio) const;
    CameraView view(float aspectRatio, float height) const;

private:
    glm::vec3 target_;
    float radius_;
    float distance_;
    float yaw_;
    float pitch_;
};
