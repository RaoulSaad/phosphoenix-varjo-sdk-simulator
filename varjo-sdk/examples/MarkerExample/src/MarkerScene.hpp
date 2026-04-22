// Copyright 2019-2020 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <Varjo.h>
#include <Varjo_world.h>

#include "Scene.hpp"


// Simple test scene consisting of markers drawn as numbered planes
class MarkerScene : public VarjoExamples::Scene
{
public:
    // Constructor
    MarkerScene(varjo_Session* session, VarjoExamples::Renderer& renderer);

    // Destructor
    ~MarkerScene();

    void LockMarkerToggle();

    void UpdateMarkerVolume(bool increase);

protected:
    // Update the tracked markers
    void onUpdate(double frameTime, double deltaTime, int64_t frameCounter, const UpdateParams& params) override;

    // Render scene to given view
    void onRender(VarjoExamples::Renderer& renderer, VarjoExamples::Renderer::ColorDepthRenderTarget& target, int viewIndex, const glm::mat4x4& viewMat,
        const glm::mat4x4& projMat, void* userData) const override;

private:
    // Struct for storing per-marker data.
    struct MarkerObject {
        glm::mat4x4 pose;  // Marker pose matrix
        glm::vec3 size;    // Marker size
        int id;            // Marker id
    };

    varjo_Session* m_session;  // Varjo session instance
    varjo_World* m_world;      // Varjo world instance

    bool m_lockMarkers{false};           // toggle to lock/unlock marker positions
    float m_markerDepthMultiplier{0.1f};  // multiplier to control marker's depth (volume)

    std::vector<MarkerObject> m_markers;                                  // List of detected markers
    std::unique_ptr<VarjoExamples::Renderer::Mesh> m_markerMesh;          // Marker mesh object instance
    std::unique_ptr<VarjoExamples::Renderer::Shader> m_markerShader;      // Marker shader instance
    std::unique_ptr<VarjoExamples::Renderer::Mesh> m_markerAxisMesh;      // Marker axis mesh objet instance
    std::unique_ptr<VarjoExamples::Renderer::Shader> m_markerAxisShader;  // Marker axis shader instance
    std::unique_ptr<VarjoExamples::Renderer::Texture> m_numberAtlas;      // Number atlas texture
};
