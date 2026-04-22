
#include "Objects.hpp"

namespace
{
// Object dimensions
constexpr float d = 1.0f;
constexpr float r = d * 0.5f;
}  // namespace

namespace Objects
{
// clang-format off

// Vertex data for cube (pos + color)
const std::vector<float> c_cubeVertexData = {
    -r, -r, -r, 0, 0, 0,
    -r, -r, r, 0, 0, 1,
    -r, r, -r, 0, 1, 0,
    -r, r, r, 0, 1, 1,
    r, -r, -r, 1, 0, 0,
    r, -r, r, 1, 0, 1,
    r, r, -r, 1, 1, 0,
    r, r, r, 1, 1, 1,
};

const int c_cubeVertexDataStride = sizeof(float) * (3 + 3);

// Index data for cube
const std::vector<unsigned int> c_cubeIndexData = {
    0, 2, 1,
    1, 2, 3,
    4, 5, 6,
    5, 7, 6,
    0, 1, 5,
    0, 5, 4,
    2, 6, 7,
    2, 7, 3,
    0, 4, 6,
    0, 6, 2,
    1, 3, 7,
    1, 7, 5,
};

// Vertex data for plane (pos + uv)
const std::vector<float> c_planeVertexData = {
    -r, 0, -r, 0, 1,
    r, 0, -r, 1, 1,
    r, 0, r, 1, 0,
    -r, 0, r, 0, 0,
};

const int c_planeVertexDataStride = sizeof(float) * (3 + 2);

// Index data for plane
const std::vector<unsigned int> c_planeIndexData = {
    0, 2, 1,
    0, 3, 2,
};
}