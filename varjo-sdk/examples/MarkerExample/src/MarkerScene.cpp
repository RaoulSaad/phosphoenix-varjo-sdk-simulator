// Copyright 2019-2020 Varjo Technologies Oy. All rights reserved.

#include "MarkerScene.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <numeric>

#include <Varjo_types_world.h>
#include "ExampleShaders.hpp"
#include "number_atlas_base64.hpp"

// VarjoExamples namespace contains simple example wrappers for using Varjo API features.
// These are only meant to be used in SDK example applications. In your own application,
// use your own production quality integration layer.
using namespace VarjoExamples;

namespace
{
constexpr std::chrono::nanoseconds c_oneSecond = std::chrono::seconds{1};
constexpr varjo_Nanoseconds c_markerLifetime = c_oneSecond.count() * 2;

// Object dimensions
constexpr float d = 1.0f;
constexpr float r = d * 0.5f;

// clang-format off
// Vertex data for marker
const std::vector<float> c_markerVertexData = {
    -r, r,  -r, 0, 0,
    r,  r,  -r, 1, 0,
    r,  r,  r,  1, 1,
    -r, r,  r,  0, 1,
    -r, 0, -r, 0, 0,
    r,  0, -r, 1, 0,
    r,  0, r,  1, 1,
    -r, 0, r,  0, 1,
};

// Index data for marker
const std::vector<unsigned int> c_markerIndexData = {
    // front
    0, 2, 1,
    0, 3, 2,
    // right
    1, 5, 6,
    6, 2, 1,
    // back
    7, 6, 5,
    5, 4, 7,
    // left
    4, 0, 3,
    3, 7, 4,
    // bottom
    4, 5, 1,
    1, 0, 4,
    // top
    3, 2, 6,
    6, 7, 3
};

// Vertex data for marker axis
const std::vector<float> c_markerAxisVertexData = {
    0, r, 0,    1, 0, 0,    -1, 0, 0,
    0, r, 0,    1, 0, 0,    1, 0, 0,
    0, r, -r,   1, 0, 0,    -1, 0, 0,
    0, r, -r,   1, 0, 0,    1, 0, 0,

    0, r, 0,    0, 1, 0,    0, 0, -1,
    0, r, 0,    0, 1, 0,    0, 0, 1,
    r, r, 0,    0, 1, 0,    0, 0, -1,
    r, r, 0,    0, 1, 0,    0, 0, 1
};

// Index data for marker axis
const std::vector<unsigned int> c_markerAxisIndexData = {
    0, 3, 1,
    0, 2, 3,

    4, 7, 5,
    4, 6, 7,
};
// clang-format on
}  // namespace

MarkerScene::MarkerScene(varjo_Session* session, Renderer& renderer)
    : m_session{session}
    , m_markerMesh(renderer.createMesh(c_markerVertexData, sizeof(float) * 5, c_markerIndexData, Renderer::PrimitiveTopology::TriangleList))
    , m_markerShader(renderer.getShaders().createShader(ExampleShaders::ShaderType::MarkerPlane))
    , m_markerAxisMesh(renderer.createMesh(c_markerAxisVertexData, sizeof(float) * 9, c_markerAxisIndexData, Renderer::PrimitiveTopology::TriangleList))
    , m_markerAxisShader(renderer.getShaders().createShader(ExampleShaders::ShaderType::MarkerAxis))
    , m_numberAtlas(renderer.loadTextureFromBase64(c_number_atlas_base64))
{
    // Initialize Varjo world with visual marker tracking enabled.
    m_world = varjo_WorldInit(m_session, varjo_WorldFlag_UseObjectMarkers);

    // Set marker lifetimes
    std::vector<varjo_WorldMarkerId> markerIds;

    // Use the marker range from https://developer.varjo.com/docs/get-started/varjo-markers
    for (varjo_WorldMarkerId id = 100; id <= 499; ++id) {
        markerIds.push_back(id);
    }

    varjo_WorldSetObjectMarkerTimeouts(m_world, markerIds.data(), markerIds.size(), c_markerLifetime);

    // By default, markers are treated as stationary (extra filtering is applied to reduce the noise in the pose).
    // Set all odd marker ids to be predicted which means they have dynamic nature and can be freely moved in space.
    std::vector<varjo_WorldMarkerId> predictedMarkerIds;
    std::copy_if(markerIds.begin(), markerIds.end(), std::back_inserter(predictedMarkerIds), [](varjo_WorldMarkerId id) { return (id % 2) == 1; });
    varjo_WorldSetObjectMarkerFlags(m_world, predictedMarkerIds.data(), predictedMarkerIds.size(), varjo_WorldObjectMarkerFlags_DoPrediction);
}

MarkerScene::~MarkerScene()
{
    // Destroy the Varjo world instance.
    varjo_WorldDestroy(m_world);
    m_world = nullptr;
    m_session = nullptr;
}

void MarkerScene::LockMarkerToggle()
{
    m_lockMarkers = !m_lockMarkers;

    LOG_INFO("Lock Markers %s", m_lockMarkers ? "ENABLED" : "DISABLED");
}

void MarkerScene::UpdateMarkerVolume(bool increase)
{
    m_markerDepthMultiplier += increase ? 0.05f : -0.05f;

    LOG_INFO("Marker volume %s to %.2f", increase ? "Increased" : "Decreased", m_markerDepthMultiplier);
}

void MarkerScene::onUpdate(double frameTime, double deltaTime, int64_t frameCounter, const UpdateParams& params)
{
    if (m_lockMarkers) {
        return;
    }

    // Update the tracking data for visual markers.
    varjo_WorldSync(m_world);

    // Get number of objects containing both varjo_WorldPoseComponent and varjo_WorldObjectMarkerComponent.
    const auto displayTime = varjo_FrameGetDisplayTime(m_session);
    const auto objectMask = varjo_WorldComponentTypeMask_Pose | varjo_WorldComponentTypeMask_ObjectMarker;
    const auto objectCount = varjo_WorldGetObjectCount(m_world, objectMask);

    if (objectCount > 0) {
        // Allocate data for all components.
        std::vector<varjo_WorldObject> objects(static_cast<size_t>(objectCount));
        varjo_WorldGetObjects(m_world, objects.data(), objectCount, objectMask);

        m_markers.clear();
        for (const auto& object : objects) {
            // Get the pose component of the object.
            // Note: pose.timeStamp value remains the same if the marker was occluded
            //
            varjo_WorldPoseComponent pose{};
            varjo_WorldGetPoseComponent(m_world, object.id, &pose, displayTime);

            // Get the object marker component of the object.
            varjo_WorldObjectMarkerComponent marker{};
            varjo_WorldGetObjectMarkerComponent(m_world, object.id, &marker);

            // Copy the pose matrix
            glm::mat4x4 matrix = fromVarjoMatrix(pose.pose);

            m_markers.push_back({
                matrix,
                {
                    static_cast<float>(marker.size.width),
                    0.0f,
                    static_cast<float>(marker.size.height),
                },
                static_cast<int>(marker.id),
            });
        }
    } else {
        m_markers.clear();
    }
}

// Render scene to given view
void MarkerScene::onRender(
    Renderer& renderer, Renderer::ColorDepthRenderTarget& target, int viewIndex, const glm::mat4x4& viewMat, const glm::mat4x4& projMat, void* userData) const
{
    // Bind the number atlas used by marker rendering
    renderer.bindTextures({m_numberAtlas.get()});

    // Render markers
    for (const auto& marker : m_markers) {
        // Render marker plane
        {
            // Calculate model transformation
            glm::mat4x4 modelMat(1.0f);
            modelMat *= marker.pose;
            auto markerSize = marker.size;
            markerSize.y = (marker.size.x + marker.size.z) * m_markerDepthMultiplier;
            modelMat = glm::scale(modelMat, markerSize);

            renderer.bindShader(*m_markerShader);

            ExampleShaders::MarkerPlaneConstants constants{};

            constants.vs.transform = ExampleShaders::TransformData(modelMat, viewMat, projMat);
            constants.ps.markerId = marker.id;

            renderer.renderMesh(*m_markerMesh, constants.vs, constants.ps);
        }

        // Render markes axis
        {
            // Calculate model transformation
            glm::mat4x4 modelMat(1.0);
            modelMat *= marker.pose;
            modelMat = glm::scale(modelMat, glm::vec3(0.8f, 1.0f, 0.8f) * marker.size);

            renderer.setDepthEnabled(false);
            renderer.bindShader(*m_markerAxisShader);

            ExampleShaders::MarkerAxisConstants constants{};
            constants.vs.transform = ExampleShaders::TransformData(modelMat, viewMat, projMat);

            renderer.renderMesh(*m_markerAxisMesh, constants.vs, constants.ps);
            renderer.setDepthEnabled(true);
        }
    }
}
