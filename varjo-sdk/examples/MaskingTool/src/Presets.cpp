
#include "Presets.hpp"

#include <fstream>

Presets::PresetId c_defaultId = "Default";

void Presets::reset()
{
    // Reset
    m_defaultId = {};
    m_resetState = {};
    m_presetIds = {};
    m_presets = {};
}

bool Presets::loadPresets(const std::string& filename)
{
    LOG_INFO("Loading presets from: %s", filename.c_str());

    reset();

    // Parse json from file
    nlohmann::json rootJson{};
    std::ifstream istr(filename);
    if (istr.good()) {
        try {
            istr >> rootJson;
        } catch (...) {
            LOG_ERROR("Parsing presets json failed.");
            return false;
        }
    } else {
        LOG_ERROR("Loading presets file failed.");
        return false;
    }

    try {
        const auto& metaJson = rootJson.at("presetMetadata");
        const auto& presetsJson = rootJson.at("presetStates");

        // Default preset id
        m_defaultId = rootJson.at("defaultId").get<std::string>();

        // Load reset state
        bool supportedPreset;
        if (!load(m_resetState, rootJson, supportedPreset, "resetState")) {
            m_resetState = {};
        }

        // Load presets
        for (size_t i = 0; i < metaJson.size(); i++) {
            const auto& metadata = metaJson[i];

            Preset preset{};
            preset.id = metadata.at("id").get<std::string>();
            preset.name = metadata.at("name").get<std::string>();
            preset.desc = metadata.at("desc").get<std::string>();

            // Reset state
            preset.state = m_resetState;

            // Load state
            if (load(preset.state, presetsJson, supportedPreset, preset.id)) {
                // Only add supported presets
                if (supportedPreset) {
                    m_presetIds.emplace_back(preset.id);
                    m_presets[preset.id] = preset;
                    LOG_INFO("Preset loaded: %s", preset.id.c_str());
                }
            } else {
                LOG_ERROR("Parsing preset failed: \"%s\"", preset.id.c_str());
                return false;
            }
        }

    } catch (...) {
        LOG_ERROR("Parsing presets failed: %s", filename.c_str());
        return false;
    }

    return true;
}

const Presets::Preset* Presets::getPreset(const PresetId& presetId) const
{
    if (m_presets.count(presetId)) {
        return &m_presets.at(presetId);
    }

    LOG_ERROR("Preset not found: \"%s\"", presetId.c_str());
    return nullptr;
}

template <typename T>
void parseOptional(const nlohmann::json& srcObj, T& destValue, const std::string& key)
{
    if (srcObj.contains(key)) {
        destValue = srcObj.at(key).get<T>();
    }
}

template <typename T>
void parseOptionalVec2(const nlohmann::json& srcObj, T& destValue, const std::string& key)
{
    if (srcObj.contains(key)) {
        const auto& arr = srcObj.at(key);
        destValue = T(arr[0], arr[1]);
    }
}

template <typename T>
void parseOptionalVec3(const nlohmann::json& srcObj, T& destValue, const std::string& key)
{
    if (srcObj.contains(key)) {
        const auto& arr = srcObj.at(key);
        destValue = T(arr[0], arr[1], arr[2]);
    }
}

template <typename T>
void parseOptionalVec4(const nlohmann::json& srcObj, T& destValue, const std::string& key)
{
    if (srcObj.contains(key)) {
        const auto& arr = srcObj.at(key);
        destValue = T(arr[0], arr[1], arr[2], arr[3]);
    }
}

template <typename T>
void parseOptionalVec16(const nlohmann::json& srcObj, T& destValue, const std::string& key)
{
    if (srcObj.contains(key)) {
        const auto& arr = srcObj.at(key);
        destValue = T(                                                      //
            (float)arr[0], (float)arr[1], (float)arr[2], (float)arr[3],     //
            (float)arr[4], (float)arr[5], (float)arr[6], (float)arr[7],     //
            (float)arr[8], (float)arr[9], (float)arr[10], (float)arr[11],   //
            (float)arr[12], (float)arr[13], (float)arr[14], (float)arr[15]  //
        );
    }
}

bool Presets::load(AppState::State& state, const nlohmann::json& rootJson, bool& supported, const std::string& name)
{
    // Set supported flag
    supported = true;

    try {
        const nlohmann::json& presetJson = name.empty() ? rootJson : rootJson.at(name);

        // Options
        if (presetJson.contains("options")) {
            const auto& optionsJson = presetJson.at("options");

            parseOptional(optionsJson, state.options.maskingMode, "maskingMode");
            parseOptional(optionsJson, state.options.vstRendering, "vstRendering");
            parseOptional(optionsJson, state.options.vrFrameSync, "vrFrameSync");
            parseOptional(optionsJson, state.options.vrFrameUpdate, "vrFrameUpdate");
            parseOptional(optionsJson, state.options.vrFrameSubmit, "vrFrameSubmit");
            parseOptional(optionsJson, state.options.vrLayerSubmitColor, "vrLayerSubmitColor");
            parseOptional(optionsJson, state.options.vrLayerSubmitMask, "vrLayerSubmitMask");
            parseOptional(optionsJson, state.options.vrLayerSubmitDepth, "vrLayerSubmitDepth");
            parseOptional(optionsJson, state.options.vrLayerDepthTestMask, "vrLayerDepthTestMask");
            parseOptional(optionsJson, state.options.vrRenderMask, "vrRenderMask");
            parseOptional(optionsJson, state.options.resDivider, "resDivider");
            parseOptional(optionsJson, state.options.frameSkip, "frameSkip");
            parseOptional(optionsJson, state.options.maskFormat, "maskFormat");
            parseOptional(optionsJson, state.options.vrViewOffset, "vrViewOffset");
            parseOptional(optionsJson, state.options.forceGlobalViewOffset, "forceGlobalViewOffset");
#ifdef USE_VIDEO_DEPTH_TEST
            parseOptional(optionsJson, state.options.videoDepthTestMode, "videoDepthTestMode");
            parseOptional(optionsJson, state.options.videoDepthTestBehavior, "videoDepthTestBehavior");
            parseOptional(optionsJson, state.options.videoDepthTestRange, "videoDepthTestRange");
#else
            // Check if trying to load unsupported preset with depth test mode
            int value = 0;
            parseOptional(optionsJson, value, "videoDepthTestMode");
            supported = (value == 0);
#endif
        }

        // Planes
        if (presetJson.contains("planes")) {
            const auto& planesJson = presetJson.at("planes");

            for (size_t i = 0; i < planesJson.size() && i < state.maskPlanes.size(); i++) {
                auto& plane = state.maskPlanes[i];

                const auto& planeJson = planesJson[i];

                parseOptional(planeJson, plane.enabled, "enabled");
                parseOptionalVec3(planeJson, plane.position, "position");
                parseOptionalVec3(planeJson, plane.rotation, "rotation");
                parseOptionalVec2(planeJson, plane.scale, "scale");
                parseOptionalVec4(planeJson, plane.color, "color");
                parseOptional(planeJson, plane.tracking, "tracking");
                parseOptional(planeJson, plane.trackedId, "trackedId");
                parseOptionalVec16(planeJson, plane.trackedPose, "trackedPose");
            }
        }
    } catch (...) {
        LOG_ERROR("Parsing preset failed: name=\"%s\"", name.c_str());
        return false;
    }

    return true;
}

bool Presets::save(const AppState::State& state, nlohmann::json& rootJson)
{
    try {
        // Options
        {
            nlohmann::json optionsJson;

            optionsJson["maskingMode"] = state.options.maskingMode;

            optionsJson["vstRendering"] = state.options.vstRendering;

            optionsJson["vrFrameSync"] = state.options.vrFrameSync;
            optionsJson["vrFrameUpdate"] = state.options.vrFrameUpdate;
            optionsJson["vrFrameSubmit"] = state.options.vrFrameSubmit;
            optionsJson["vrLayerSubmitColor"] = state.options.vrLayerSubmitColor;
            optionsJson["vrLayerSubmitMask"] = state.options.vrLayerSubmitMask;
            optionsJson["vrLayerSubmitDepth"] = state.options.vrLayerSubmitDepth;
            optionsJson["vrLayerDepthTestMask"] = state.options.vrLayerDepthTestMask;
            optionsJson["vrRenderMask"] = state.options.vrRenderMask;
            optionsJson["resDivider"] = state.options.resDivider;
            optionsJson["frameSkip"] = state.options.frameSkip;
            optionsJson["maskFormat"] = state.options.maskFormat;
            optionsJson["vrViewOffset"] = state.options.vrViewOffset;
            optionsJson["forceGlobalViewOffset"] = state.options.forceGlobalViewOffset;
#ifdef USE_VIDEO_DEPTH_TEST
            optionsJson["videoDepthTestMode"] = state.options.videoDepthTestMode;
            optionsJson["videoDepthTestBehavior"] = state.options.videoDepthTestBehavior;
            optionsJson["videoDepthTestRange"] = state.options.videoDepthTestRange;
#endif
            rootJson["options"] = optionsJson;
        }

        // Plane configs
        {
            nlohmann::json planesJson;
            for (const auto& plane : state.maskPlanes) {
                nlohmann::json planeJson;
                planeJson["enabled"] = plane.enabled;
                planeJson["position"] = {plane.position.x, plane.position.y, plane.position.z};
                planeJson["rotation"] = {plane.rotation.x, plane.rotation.y, plane.rotation.z};
                planeJson["scale"] = {plane.scale.x, plane.scale.y};
                planeJson["color"] = {plane.color.r, plane.color.g, plane.color.b, plane.color.a};
                planeJson["tracking"] = false;  // plane.tracking; We don't save tracking state. Always false.
                planeJson["trackedId"] = plane.trackedId;
                planeJson["trackedPose"] = {
                    plane.trackedPose[0][0], plane.trackedPose[0][1], plane.trackedPose[0][2], plane.trackedPose[0][3],  //
                    plane.trackedPose[1][0], plane.trackedPose[1][1], plane.trackedPose[1][2], plane.trackedPose[1][3],  //
                    plane.trackedPose[2][0], plane.trackedPose[2][1], plane.trackedPose[2][2], plane.trackedPose[2][3],  //
                    plane.trackedPose[3][0], plane.trackedPose[3][1], plane.trackedPose[3][2], plane.trackedPose[3][3],  //
                };

                planesJson.push_back(planeJson);
            }

            rootJson["planes"] = planesJson;
        }
    } catch (...) {
        LOG_ERROR("Serializing preset failed.");
        return false;
    }

    return true;
}

bool Presets::loadState(const std::string& filename, AppState::State& state)
{
    LOG_INFO("Load state from: %s", filename.c_str());

    // Parse json from file
    nlohmann::json rootJson{};
    std::ifstream istr(filename);
    if (istr.good()) {
        try {
            istr >> rootJson;
        } catch (...) {
            LOG_ERROR("Parsing state from json failed.");
            return false;
        }
    } else {
        LOG_ERROR("Opening file for reading failed: %s", filename.c_str());
        return false;
    }

    // Parse state from json
    bool supportedPreset;  // Ignored here, only used for preset loading
    if (!Presets::load(state, rootJson, supportedPreset)) {
        LOG_ERROR("Parsing state failed.");
        return false;
    }

    return true;
}

bool Presets::saveState(const std::string& filename, const AppState::State& state)
{
    LOG_INFO("Save state to: %s", filename.c_str());

    // Create json object from state
    nlohmann::json rootJson;
    if (!Presets::save(state, rootJson)) {
        LOG_ERROR("Saving state failed.");
        return false;
    }

    // Write to file stream
    std::ofstream ostr(filename);
    if (ostr.good()) {
        ostr << rootJson;
    } else {
        LOG_ERROR("Opening file for writing failed: %s", filename.c_str());
        return false;
    }

    return true;
}