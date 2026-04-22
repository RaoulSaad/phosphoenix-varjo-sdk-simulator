#pragma once

#include "IRenderer.hpp"

#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>
#include <string>
#include <cstdio>
#include <Windows.h>
#include <openvr.h>
#include <unordered_map>

struct Trackable {
    vr::TrackedDevicePose_t pose;
    glm::vec3 position;
    glm::quat orientation;
    std::shared_ptr<Geometry> renderModel;
};

class OpenVRTracker
{
public:
    OpenVRTracker(IRenderer& renderer, const std::shared_ptr<Geometry>& defaultTrackableModel);
    ~OpenVRTracker();

    void init();
    void update(float timeToDisplay);
    void exit();

    int getTrackableCount() const { return static_cast<int>(m_trackables.size()); }
    glm::vec3 getTrackablePosition(int index) const { return m_trackables.at(index).position; }
    glm::quat getTrackableOrientation(int index) const { return m_trackables.at(index).orientation; }
    std::shared_ptr<Geometry> getTrackableRenderModel(int index) const { return m_trackables.at(index).renderModel; }

private:
    std::shared_ptr<Geometry> getOrLoadRenderModel(vr::TrackedDeviceIndex_t unDevice);

private:
    vr::IVRSystem* m_vrSystem{nullptr};
    std::vector<Trackable> m_trackables;

    IRenderer& m_renderer;
    std::shared_ptr<Geometry> m_defaultTrackableModel;
    std::unordered_map<std::string, std::shared_ptr<Geometry>> m_renderModelMap;
};
