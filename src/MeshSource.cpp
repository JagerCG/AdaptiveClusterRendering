#include "MeshSource.h"

MeshSource makeCube()
{
    MeshSource cube;
    // Each face has its own vertices so the normals stay flat across the face.
    cube.vertices = {
        {{-1, -1,  1}, { 0,  0,  1}},
        {{ 1, -1,  1}, { 0,  0,  1}},
        {{ 1,  1,  1}, { 0,  0,  1}},
        {{-1,  1,  1}, { 0,  0,  1}},

        {{ 1, -1,  1}, { 1,  0,  0}},
        {{ 1, -1, -1}, { 1,  0,  0}},
        {{ 1,  1, -1}, { 1,  0,  0}},
        {{ 1,  1,  1}, { 1,  0,  0}},

        {{-1,  1,  1}, { 0,  1,  0}},
        {{ 1,  1,  1}, { 0,  1,  0}},
        {{ 1,  1, -1}, { 0,  1,  0}},
        {{-1,  1, -1}, { 0,  1,  0}},

        {{-1, -1, -1}, {-1,  0,  0}},
        {{-1, -1,  1}, {-1,  0,  0}},
        {{-1,  1,  1}, {-1,  0,  0}},
        {{-1,  1, -1}, {-1,  0,  0}},

        {{-1, -1, -1}, { 0, -1,  0}},
        {{ 1, -1, -1}, { 0, -1,  0}},
        {{ 1, -1,  1}, { 0, -1,  0}},
        {{-1, -1,  1}, { 0, -1,  0}},

        {{ 1, -1, -1}, { 0,  0, -1}},
        {{-1, -1, -1}, { 0,  0, -1}},
        {{-1,  1, -1}, { 0,  0, -1}},
        {{ 1,  1, -1}, { 0,  0, -1}}
    };

    cube.indices = {
         0,  1,  2,  0,  2,  3,
         4,  5,  6,  4,  6,  7,
         8,  9, 10,  8, 10, 11,
        12, 13, 14, 12, 14, 15,
        16, 17, 18, 16, 18, 19,
        20, 21, 22, 20, 22, 23
    };
    for (const Vertex& vertex : cube.vertices) {
        const uint32_t x = vertex.position.x > 0.0f ? 1u : 0u;
        const uint32_t y = vertex.position.y > 0.0f ? 1u : 0u;
        const uint32_t z = vertex.position.z > 0.0f ? 1u : 0u;
        cube.positionIds.push_back(x + 2 * y + 4 * z);
    }
    return cube;
}
