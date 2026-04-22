// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <memory>
#include <array>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>

#include "Globals.hpp"
#include "Renderer.hpp"
#include "Scene.hpp"

#include "Objects.hpp"
#include "AppState.hpp"

//! Simple masking scene for modifying chroma key mask in post process
class MaskScene : public VarjoExamples::Scene
{
public:
    //! Constructor
    MaskScene(VarjoExamples::Renderer& renderer);

    //! Update plane data
    void updatePlanes(const std::array<AppState::PlaneConfig, AppState::NumMaskPlanes>& planeConfigs);

protected:
    //! Update scene animation
    void onUpdate(double frameTime, double deltaTime, int64_t frameCounter, const UpdateParams& params) override;

    //! Render scene to given view
    void onRender(VarjoExamples::Renderer& renderer, VarjoExamples::Renderer::ColorDepthRenderTarget& target, int viewIndex, const glm::mat4x4& varjoViewMat,
        const glm::mat4x4& varjoProjMat, void* userData) const override;

private:
    struct Plane {
        Object object{};
        glm::mat4x4 trackedPose{1.0f};
        bool enabled = false;
    };

    std::array<Plane, AppState::NumMaskPlanes> m_planes;             //!< Plane objects
    std::unique_ptr<VarjoExamples::Renderer::Shader> m_planeShader;  //!< Plane shader instance
    std::unique_ptr<VarjoExamples::Renderer::Mesh> m_planeMesh;      //!< Mesh object instance
};
