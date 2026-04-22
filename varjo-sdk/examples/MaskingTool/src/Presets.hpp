#pragma once

#include <unordered_map>

#include <json/json.hpp>

#include "Globals.hpp"

#include "AppState.hpp"

class Presets
{
public:
    //! Preset id type
    using PresetId = std::string;

    //! Preset metadata and data
    struct Preset {
        PresetId id;            //!< Preset id
        std::string name;       //!< Display name
        std::string desc;       //!< Description text
        AppState::State state;  //!< Preset state
        bool supported{true};   //!< Preset supported flag
    };

    //! Constructor
    Presets() = default;

    //! Reset all presets
    void reset();

    //! Load presets from filename
    bool loadPresets(const std::string& filename);

    //! Return default preset id
    const PresetId& getDefaultId() const { return m_defaultId; }

    //! Return reset state
    const AppState::State& getResetState() const { return m_resetState; }

    //! Return preset id by index
    const PresetId& getPresetId(size_t index) const { return m_presetIds[index]; }

    //! Return number of presets
    size_t getPresetCount() const { return m_presetIds.size(); }

    //! Return preset by id
    const Preset* getPreset(const PresetId& presetId) const;

    //! Load state from given json. If name is not given, load directly from root object
    static bool load(AppState::State& state, const nlohmann::json& rootJson, bool& supported, const std::string& name = "");

    //! Save state to given json.
    static bool save(const AppState::State& state, nlohmann::json& rootJson);

    //! Load state from given file
    static bool loadState(const std::string& filename, AppState::State& state);

    //! Save state to given file
    static bool saveState(const std::string& filename, const AppState::State& state);

private:
    AppState::State m_resetState{};                  //!< Reset state
    PresetId m_defaultId;                            //!< Default preset id
    std::vector<PresetId> m_presetIds;               //!< Listed preset ids
    std::unordered_map<PresetId, Preset> m_presets;  //!< Preset data
};
