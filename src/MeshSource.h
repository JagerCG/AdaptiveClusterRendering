#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texcoord{0.0f};
};

struct MeshSource {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // Split vertices keep the same source position ID across UV and normal seams.
    std::vector<uint32_t> positionIds;
};

MeshSource makeCube();
