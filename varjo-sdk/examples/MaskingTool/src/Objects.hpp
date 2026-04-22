#pragma once

#include <vector>

#include "Globals.hpp"
#include "Scene.hpp"

template <typename T>
std::array<T, 3> glmToArray(const glm::vec3& v)
{
    return {v.x, v.y, v.z};
}

template <typename T>
std::array<T, 4> glmToArray(const glm::vec4& v)
{
    return {v.x, v.y, v.z, v.w};
}

template <typename T>
std::array<T, 9> glmToArray(const glm::mat3x3& m)
{
    return {m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]};
}

template <typename T>
std::array<T, 16> glmToArray(const glm::mat4x4& m)
{
    return {m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]};
}

//! Simple struct for storing object data
struct Object {
    VarjoExamples::ObjectPose pose;              //!< Object pose
    glm::vec4 color = {1.0f, 1.0f, 1.0f, 1.0f};  //!< Object rgb + alpha
};

namespace Objects
{
extern const std::vector<float> c_cubeVertexData;
extern const int c_cubeVertexDataStride;
extern const std::vector<unsigned int> c_cubeIndexData;

extern const std::vector<float> c_planeVertexData;
extern const int c_planeVertexDataStride;
extern const std::vector<unsigned int> c_planeIndexData;
}  // namespace Objects
