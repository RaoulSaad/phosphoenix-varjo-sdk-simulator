
#include "OpenVRTracker.hpp"

// Get the quaternion representing the orientation
glm::quat getOrientation(const vr::HmdMatrix34_t matrix)
{
    glm::quat q;

    q.w = static_cast<float>(sqrt(fmax(0, 1 + matrix.m[0][0] + matrix.m[1][1] + matrix.m[2][2])) / 2);
    q.x = static_cast<float>(sqrt(fmax(0, 1 + matrix.m[0][0] - matrix.m[1][1] - matrix.m[2][2])) / 2);
    q.y = static_cast<float>(sqrt(fmax(0, 1 - matrix.m[0][0] + matrix.m[1][1] - matrix.m[2][2])) / 2);
    q.z = static_cast<float>(sqrt(fmax(0, 1 - matrix.m[0][0] - matrix.m[1][1] + matrix.m[2][2])) / 2);
    q.x = static_cast<float>(copysign(q.x, matrix.m[2][1] - matrix.m[1][2]));
    q.y = static_cast<float>(copysign(q.y, matrix.m[0][2] - matrix.m[2][0]));
    q.z = static_cast<float>(copysign(q.z, matrix.m[1][0] - matrix.m[0][1]));
    return q;
}

// Get the vector representing the position
glm::vec3 getPosition(const vr::HmdMatrix34_t matrix)
{
    glm::vec3 vector;
    vector.x = matrix.m[0][3];
    vector.y = matrix.m[1][3];
    vector.z = matrix.m[2][3];
    return vector;
}

OpenVRTracker::OpenVRTracker(IRenderer& renderer, const std::shared_ptr<Geometry>& defaultTrackableModel)
    : m_renderer(renderer)
    , m_defaultTrackableModel(defaultTrackableModel)
{
}

OpenVRTracker::~OpenVRTracker() { exit(); }

void OpenVRTracker::exit()
{
    if (m_vrSystem) {
        vr::VR_Shutdown();
        m_vrSystem = nullptr;
    }
}

std::shared_ptr<Geometry> OpenVRTracker::getOrLoadRenderModel(vr::TrackedDeviceIndex_t unDevice)
{
    std::string renderModelName;
    const uint32_t stringSize = vr::VRSystem()->GetStringTrackedDeviceProperty(unDevice, vr::Prop_RenderModelName_String, nullptr, 0);

    if (stringSize == 0) {
        return m_defaultTrackableModel;
    }

    renderModelName.resize(stringSize - 1);
    vr::VRSystem()->GetStringTrackedDeviceProperty(unDevice, vr::Prop_RenderModelName_String, renderModelName.data(), stringSize);

    vr::RenderModel_t* renderModel;
    vr::EVRRenderModelError error = vr::VRRenderModels()->LoadRenderModel_Async(renderModelName.data(), &renderModel);

    // Return default model if loading fails or is not complete yet
    if (error != vr::VRRenderModelError_None) {
        return m_defaultTrackableModel;
    }

    auto iter = m_renderModelMap.find(renderModelName);
    if (iter != m_renderModelMap.end()) return iter->second;


    const int32_t vertexCount = renderModel->unVertexCount;
    const int32_t indexCount = renderModel->unTriangleCount * 3;

    std::vector<Geometry::Vertex> vertices(vertexCount);
    std::vector<uint32_t> indices(indexCount);

    // Transform rendermodel vertex/index buffers to our format
    {
        for (size_t i = 0; i < vertices.size(); ++i) {
            vertices[i].position = {
                renderModel->rVertexData[i].vPosition.v[0], renderModel->rVertexData[i].vPosition.v[1], renderModel->rVertexData[i].vPosition.v[2]};
            vertices[i].normal = {renderModel->rVertexData[i].vNormal.v[0], renderModel->rVertexData[i].vNormal.v[1], renderModel->rVertexData[i].vNormal.v[2]};
        }

        for (size_t i = 0; i < indices.size(); ++i) {
            indices[i] = static_cast<uint32_t>(renderModel->rIndexData[i]);
        }
    }

    std::shared_ptr<Geometry> geometry = m_renderer.createGeometry(vertexCount, indexCount);
    geometry->updateVertexBuffer(vertices.data());
    geometry->updateIndexBuffer(indices.data());

    m_renderModelMap.insert(std::make_pair(renderModelName, geometry));

    return geometry;
}

void OpenVRTracker::init()
{
    if (m_vrSystem) return;

    if (!vr::VR_IsRuntimeInstalled()) {
        printf("No runtime installed\n");
        return;
    }

    if (!vr::VR_IsHmdPresent()) {
        printf("No Hmd Present\n");
        return;
    }

    vr::EVRInitError evrInitError = vr::EVRInitError::VRInitError_None;
    m_vrSystem = vr::VR_Init(&evrInitError, vr::EVRApplicationType::VRApplication_Other, nullptr);

    if (evrInitError != vr::EVRInitError::VRInitError_None) {
        printf("Failed to initialize openVR\n");
        m_vrSystem = nullptr;
    } else {
        printf("OpenVR Initialized.\n");
    }
}

void OpenVRTracker::update(float timeToDisplay)
{
    m_trackables.clear();

    if (!m_vrSystem) return;

    std::vector<vr::TrackedDevicePose_t> poses(vr::k_unMaxTrackedDeviceCount);
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(
        vr::TrackingUniverseOrigin::TrackingUniverseStanding, timeToDisplay, poses.data(), vr::k_unMaxTrackedDeviceCount);

    for (vr::TrackedDeviceIndex_t unDevice = 1; unDevice < vr::k_unMaxTrackedDeviceCount; unDevice++) {
        if (!m_vrSystem->IsTrackedDeviceConnected(unDevice)) continue;

        vr::TrackedDevicePose_t trackedDevicePose = poses[unDevice];

        if (trackedDevicePose.bDeviceIsConnected && trackedDevicePose.bPoseIsValid &&
            trackedDevicePose.eTrackingResult == vr::ETrackingResult::TrackingResult_Running_OK) {
            Trackable trackable;
            trackable.pose = trackedDevicePose;
            trackable.position = getPosition(trackedDevicePose.mDeviceToAbsoluteTracking);
            trackable.orientation = getOrientation(trackedDevicePose.mDeviceToAbsoluteTracking);

            // Queries devices rendermodel name every frame and caches
            // loaded models.
            // TODO: It is possible to query individual rendermodel components
            // and their state to draw animated buttons etc.
            trackable.renderModel = getOrLoadRenderModel(unDevice);

            m_trackables.push_back(trackable);
        }
    }
}