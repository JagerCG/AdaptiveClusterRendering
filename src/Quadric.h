#pragma once

#include <glm/vec3.hpp>
#include <array>
#include <cstddef>

struct Quadric {
    std::array<double, 10> values{};

    void addPlane(double a, double b, double c, double d)
    {
        const double plane[] = {a, b, c, d};
        size_t index = 0;
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = row; column < 4; ++column) {
                values[index++] += plane[row] * plane[column];
            }
        }
    }

    Quadric& operator+=(const Quadric& other)
    {
        for (size_t i = 0; i < values.size(); ++i)
            values[i] += other.values[i];
        return *this;
    }

    double evaluate(const glm::dvec3& point) const
    {
        const double x = point.x, y = point.y, z = point.z;
        return values[0] * x * x + 2.0 * values[1] * x * y + 2.0 * values[2] * x * z + 2.0 * values[3] * x +
               values[4] * y * y + 2.0 * values[5] * y * z + 2.0 * values[6] * y + values[7] * z * z +
               2.0 * values[8] * z + values[9];
    }
};
