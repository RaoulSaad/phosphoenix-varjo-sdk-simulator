// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#include "MaskScene.hpp"
#include "D3D11Renderer.hpp"
#include "D3D11Shaders.hpp"

// VarjoExamples namespace contains simple example wrappers for using Varjo API features.
// These are only meant to be used in SDK example applications. In your own application,
// use your own production quality integration layer.
using namespace VarjoExamples;

namespace
{
const char* c_planeVSSource = R"src(
cbuffer ConstantBuffer : register(b0) {
    matrix modelMat;
    matrix viewMat;
    matrix projMat;
};

struct VsInput {
    float3 pos : POSITION;
    float2 texCoord : TEXCOORD0;
};

struct VsOutput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VsOutput main(in VsInput input) {
    VsOutput output;
    float4 pos = float4(input.pos, 1.0f);

    // Transform vertex
    pos = mul(modelMat, pos);
    pos = mul(viewMat, pos);
    pos = mul(projMat, pos);

    // Write output
    output.position = pos;
    output.texCoord = input.texCoord.xy;
    return output;
}
)src";

const char* c_planePSSource = R"src(

cbuffer ConstantBuffer : register(b0) {
    float4 color;
};

struct PsInput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

float4 main(PsInput input) : SV_TARGET {
    float4 output = color;
    return output;
}
)src";

//! Rainbow cube shader constants
struct PlaneShaderConstants {
    //! Vertex shader constants
    struct {
        ExampleShaders::TransformData transform;
    } vs;

    //! Pixel shade contants
    struct {
        glm::vec4 objectColor;
    } ps;

    // Check constant buffer sizes
    static_assert(sizeof(vs) % 16 == 0, "Invalid constant buffer size.");
    static_assert(sizeof(ps) % 16 == 0, "Invalid constant buffer size.");
};

// Shader init params
const D3D11Renderer::Shader::InitParams c_planeShaderParams = {
    "MaskPlane",
    c_planeVSSource,
    c_planePSSource,
    sizeof(PlaneShaderConstants::vs),
    sizeof(PlaneShaderConstants::ps),
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    },
};

}  // namespace

MaskScene::MaskScene(Renderer& renderer)
    : m_planeMesh(renderer.createMesh(
          Objects::c_planeVertexData, Objects::c_planeVertexDataStride, Objects::c_planeIndexData, Renderer::PrimitiveTopology::TriangleList))
{
    // Create shader
    D3D11Renderer& d3d11Renderer = reinterpret_cast<D3D11Renderer&>(renderer);
    m_planeShader = d3d11Renderer.createShader(c_planeShaderParams);
}

void MaskScene::updatePlanes(const std::array<AppState::PlaneConfig, AppState::NumMaskPlanes>& planeConfigs)
{
    int i = 0;
    for (const auto& planeConf : planeConfigs) {
        m_planes[i].enabled = planeConf.enabled;
        m_planes[i].object.pose.position = planeConf.position;
        m_planes[i].object.pose.scale = glm::vec3(planeConf.scale.x, 1.0f, planeConf.scale.y);

        glm::vec3 angles(planeConf.rotation.x, planeConf.rotation.y, planeConf.rotation.z);
        angles *= glm::pi<float>() / 180.0f;
        m_planes[i].object.pose.rotation = glm::quat(angles);

        m_planes[i].trackedPose = planeConf.trackedPose;
        m_planes[i].object.color = planeConf.color;

        // Disable planes that are tracked but not assigned
        if (planeConf.tracking && planeConf.trackedId <= 0) {
            m_planes[i].enabled = false;
        }

        i++;
    }
}

void MaskScene::onUpdate(double frameTime, double deltaTime, int64_t frameCounter, const UpdateParams& params)
{
    // Update scene
}

void MaskScene::onRender(Renderer& renderer, Renderer::ColorDepthRenderTarget& target, int viewIndex, const glm::mat4x4& varjoViewMat,
    const glm::mat4x4& varjoProjMat, void* userData) const
{
    // Bind the plane shader
    renderer.bindShader(*m_planeShader);

    // Render planes
    for (const auto& plane : m_planes) {
        if (plane.enabled) {
            const auto& object = plane.object;
            // Calculate model transformation
            glm::mat4x4 modelMat(1.0);
            modelMat *= plane.trackedPose;
            modelMat = glm::translate(modelMat, object.pose.position);
            glm::translate(modelMat, object.pose.position);
            modelMat *= glm::toMat4(object.pose.rotation);
            modelMat = glm::scale(modelMat, object.pose.scale);

            // Constants
            PlaneShaderConstants constants;
            constants.vs.transform = ExampleShaders::TransformData(modelMat, varjoViewMat, varjoProjMat);
            constants.ps.objectColor = object.color;

            // Render mesh
            renderer.renderMesh(*m_planeMesh, constants.vs, constants.ps);
        }
    }
}
