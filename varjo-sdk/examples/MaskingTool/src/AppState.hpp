
// Copyright 2020-2021 Varjo Technologies Oy. All rights reserved.

#pragma once

#include <glm/glm.hpp>

#include "Globals.hpp"
#include "MarkerTracker.hpp"

// Video Depth Test API is in Experimental SDK, so it won't be available if build from regular SDK.
#ifdef USE_EXPERIMENTAL_API
#define USE_VIDEO_DEPTH_TEST
#endif

//! Application state struct
struct AppState {
    //! Number of masking planes
    static const int NumMaskPlanes{4};

    // None:
    // - Chroma keying everywhere, rendered mask ignored.
    //
    // Restricted mode:
    // - Masked area: Use chroma keying.
    // - Non-masked area: Always show VR.
    //
    // Extended mode:
    // - Masked area: Always show VR.
    // - Non-masked area: Use chroma keying.
    //
    // Reduced mode:
    // - Masked area: Always show video-pass-through.
    // - Non-masked area: Use chroma keying.

    //! Masking modes enumeration
    enum class MaskMode {
        None = 0,             //!< No masking done
        Restricted = 1,       //!< Restricted to masked areas
        Extended = 2,         //!< Extended by masked areas
        Reduced = 3,          //!< Reduced by masked areas
        DepthTestOrFail = 4,  //!< Depth test on masked area, fail outside
        DepthTestOrPass = 5,  //!< Depth test on masked area, pass outside
    };

    //! Debug visualization modes enumeration
    enum class DebugMode {
        None = 0,             //!< No debug output
        VisualizeMask = 1,    //!< Visualize mask alpha
        VisualizeColors = 2,  //!< Visualize masking planes in colors
    };

#ifdef USE_VIDEO_DEPTH_TEST

    //! Video depth test modes
    enum class VideoDepthTestMode {
        Default = 0,       //!< Use system default mode and range
        FullRange = 1,     //!< Video depth test range not limited
        LimitedRange = 2,  //!< Video depth test range limited to given values
        ForcedRange = 3,   //!< Video depth test globally enabled. Fixed depth range far Z used if no application depth
    };

    //! Video depth test range combine behavior
    enum class VideoDepthTestBehavior {
        PreferLayerRange = 0,  //!< Prefer application layer depth test range
        PreferVideoRange = 1,  //!< Prefer global video depth test range
        CombineRanges = 2,     //!< Apply both ranges
    };

#endif

    //! General params structure
    struct General {
        AppState::DebugMode debugMode{AppState::DebugMode::None};  //!< Debug visualization mode
        double frameTime{0.0f};                                    //!< Current frame time
        int64_t frameCount{0};                                     //!< Current frame count
        bool mrAvailable{false};                                   //!< Mixed reality available flag
        bool vstDepthEstimation{false};                            //!< Estimate VST depth flag
    };

    //! Options structure
    struct Options {
        MaskMode maskingMode{MaskMode::Extended};  //!< Masking mode

        // VST feature options
        bool vstRendering{true};  //!< Render VST image flag

        // VR layer options
        bool vrFrameSync{true};                                            //!< VR frame sync
        bool vrFrameUpdate{true};                                          //!< VR frame update
        bool vrFrameSubmit{true};                                          //!< VR frame submit
        bool vrLayerSubmitColor{false};                                    //!< VR layer submit: color layer
        bool vrLayerSubmitMask{true};                                      //!< VR layer submit: mask layer
        bool vrLayerSubmitDepth{false};                                    //!< VR layer depth submit flag (for both color and mask)
        bool vrLayerDepthTestMask{false};                                  //!< VR layer: depth test masking layer against VST depth
        bool vrRenderMask{true};                                           //!< VR scene rendering
        int resDivider{2};                                                 //!< Mask buffer resolution divider
        int frameSkip{1};                                                  //!< Number of frames skipped before next submit
        varjo_TextureFormat maskFormat{varjo_MaskTextureFormat_A8_UNORM};  //!< Mask buffer format
        float vrViewOffset{1.0f};                                          //!< Mask VR view offset
        bool forceGlobalViewOffset{true};                                  //!< Force VR view offset for all clients

#ifdef USE_VIDEO_DEPTH_TEST
        // Depth testing
        VideoDepthTestMode videoDepthTestMode{VideoDepthTestMode::Default};                       //!< Global video depth test mode
        VideoDepthTestBehavior videoDepthTestBehavior{VideoDepthTestBehavior::PreferLayerRange};  //!< Global video depth test behavior
        std::array<float, 2> videoDepthTestRange{0.0f, 0.75f};                                    //!< Global video depth test range
#endif
    };

    //! Config structure for masking plane
    struct PlaneConfig {
        bool enabled{false};                      //!< Plane enable flag
        glm::vec3 position{0.0f, 0.0f, 0.0f};     //!< Plane position
        glm::vec3 rotation{0.0f, 0.0f, 0.0f};     //!< Plane rotation angles
        glm::vec2 scale{1.0f, 1.0f};              //!< Plane scale
        glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  //!< Plane color
        bool tracking{false};                     //!< Live tracking enabled flag
        bool resetMarkerPrediction{false};        //!< Tracking marker prediction reset requested flag
        int trackedId{0};                         //!< Tracked marker id
        glm::mat4x4 trackedPose{1.0f};            //!< Tracked pose
    };

    //! App state struct
    struct State {
        Options options{};                                    //!< Options state
        std::array<PlaneConfig, NumMaskPlanes> maskPlanes{};  //!< Masking plane states
    };

    General general{};  //!< General params
    State state{};      //!< Current storable state
};
