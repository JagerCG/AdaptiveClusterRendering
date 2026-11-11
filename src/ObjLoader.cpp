#include "ObjLoader.h"

#include <tiny_obj_loader.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

glm::vec3 readVec3(const std::vector<tinyobj::real_t>& values, int index, const char* attribute)
{
    if (index < 0 || static_cast<size_t>(index) >= values.size() / 3) {
        throw std::runtime_error(std::string("Invalid OBJ ") + attribute + " index.");
    }

    const size_t offset = static_cast<size_t>(index) * 3;
    const glm::vec3 value{values[offset], values[offset + 1], values[offset + 2]};
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
        throw std::runtime_error(std::string("Invalid OBJ ") + attribute + " value.");
    }
    return value;
}

glm::vec2 readTexcoord(const tinyobj::attrib_t& attributes, int index)
{
    if (index < 0) {
        return glm::vec2(0.0f);
    }
    if (static_cast<size_t>(index) >= attributes.texcoords.size() / 2) {
        throw std::runtime_error("Invalid OBJ texture coordinate index.");
    }
    const size_t offset = static_cast<size_t>(index) * 2;
    const glm::vec2 texcoord{attributes.texcoords[offset], attributes.texcoords[offset + 1]};
    if (!std::isfinite(texcoord.x) || !std::isfinite(texcoord.y)) {
        throw std::runtime_error("Invalid OBJ texture coordinate.");
    }
    return texcoord;
}

} // namespace

MeshSource loadObj(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Cannot open OBJ file: " + path.u8string());
    }

    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string error;
    if (!tinyobj::LoadObj(&attributes, &shapes, &materials, &warning, &error, &file, nullptr)) {
        throw std::runtime_error("Cannot read OBJ file: " + path.u8string() + "\n" + error);
    }
    if (!warning.empty()) {
        std::cerr << warning;
    }

    MeshSource mesh;
    // An OBJ position can be split by a UV seam or a hard normal.
    std::map<std::tuple<int, int, int>, uint32_t> vertexIndices;
    for (const auto& shape : shapes) {
        const auto& corners = shape.mesh.indices;
        if (corners.size() % 3 != 0 ||
            std::any_of(shape.mesh.num_face_vertices.begin(), shape.mesh.num_face_vertices.end(),
                        [](unsigned int count) { return count != 3; })) {
            throw std::runtime_error("OBJ contains a face that could not be triangulated.");
        }
        if (corners.size() > std::numeric_limits<uint32_t>::max() - mesh.indices.size()) {
            throw std::runtime_error("OBJ has too many triangle indices.");
        }

        for (size_t i = 0; i < corners.size(); i += 3) {
            const auto* triangle = &corners[i];
            glm::vec3 positions[3];
            for (size_t j = 0; j < 3; ++j) {
                positions[j] = readVec3(attributes.vertices, triangle[j].vertex_index, "position");
            }
            const glm::vec3 cross = glm::cross(positions[1] - positions[0], positions[2] - positions[0]);
            const float length = glm::length(cross);
            const glm::vec3 faceNormal = length > 0.0f && std::isfinite(length)
                ? cross / length
                : glm::vec3(0.0f, 1.0f, 0.0f);

            for (size_t j = 0; j < 3; ++j) {
                const auto& corner = triangle[j];
                Vertex vertex{positions[j], faceNormal, readTexcoord(attributes, corner.texcoord_index)};
                const auto vertexIndex = static_cast<uint32_t>(mesh.vertices.size());
                if (corner.normal_index >= 0) {
                    const glm::vec3 normal = readVec3(attributes.normals, corner.normal_index, "normal");
                    if (glm::dot(normal, normal) > 0.0f) {
                        const auto key = std::make_tuple(corner.vertex_index, corner.normal_index, corner.texcoord_index);
                        const auto [entry, inserted] = vertexIndices.try_emplace(key, vertexIndex);
                        if (!inserted) {
                            mesh.indices.push_back(entry->second);
                            continue;
                        }
                        vertex.normal = normal;
                    }
                }

                mesh.indices.push_back(vertexIndex);
                mesh.vertices.push_back(vertex);
                mesh.positionIds.push_back(static_cast<uint32_t>(corner.vertex_index));
            }
        }
    }
    if (mesh.indices.empty()) {
        throw std::runtime_error("OBJ has no triangle faces: " + path.u8string());
    }
    return mesh;
}
