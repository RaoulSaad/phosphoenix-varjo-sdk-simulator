# Varjo SDK Changelog

This file describes changes in Varjo SDK. See RUNTIME_CHANGELOG.md for
changes in the runtime behavior.

## 4.6

### Added

- Added `varjo_CameraIntrinsics2` struct. Newly added `varjo_GetCameraIntrinsics2()`
  can be used for querying this struct.
- Added VST undistortion example for rational camera model to MRExample.

### Deprecated

- Deprecated `varjo_CameraIntrinsics` struct and `varjo_GetCameraIntrinsics()`
  function to achieve wider support for camera models.

## 4.4

### Added

- Added locking feature to MarkerExample.
  Pressing SPACE bar locks current positions of all detected markers.
  Pressing SPACE bar again unlocks positions and returns to normal mode.
- Added control for rendered marker Volume (depth).
  Pressing UP key increases the depth of rendered 3D markers.
  Pressing DOWN key decreases the depth up to making them flat (2D plane).

### Changed

- Changed all const char* globals in Varjo_types.h to be unassignable.
- Compatibility updates to Varjo_export.h.

## 4.0

### Added

- Added a new event type `varjo_EventType_VisibilityMeshChange` which is
  triggered when the visibility mesh has changed for view(s). Benchmark
  example shows the usage pattern of this event. Applications can listen
  to this event and accordingly re-query the visibility mesh through
  api `varjo_CreateOcclusionMesh` to apply new mesh for rendering.
- Added a new camera property `varjo_CameraPropertyType_FocusDistance` to
  control the focus distance. Only HMDs with autofocus cameras support this feature. 
  "AUTO" and "MANUAL" modes are available to set focus distance.
- Added a new camera property `varjo_CameraPropertyType_AutoExposureBehavior`
  to control how VST auto exposure reacts to changes in lighting and environment.
  `varjo_AutoExposureBehavior_Normal` offers the original auto exposure behavior,
  while `varjo_AutoExposureBehavior_PreventOverexposure` introduces a new
  more aggressive behavior, which aims to prevent any oversaturation in the
  image.

## 3.10.0

### Deprecated

- Deprecated Legacy 10-dot gaze calibration. Newer Fast 5-dot gaze calibration
  allows to achieve higher accuracy and precision. Requesting Legacy 10-dot gaze
  calibration will trigger Fast 5-dot calibration.

### Removed

- Removed legacy rendering API functions: `varjo_D3D11Init`, `varjo_D3D11Shutdown`, 
`varjo_GLInit`, `varjo_GLShutdown`


## 3.9.0

### Changed

- SDK example builds now use Visual Studio 2019 instead of 2017. Build scripts
  and readme file updated accordingly.

## 3.8.0

### Added

- Added support for setting headset IPD parameters via the
  `varjo_SetInterPupillaryDistanceParameters()` function. The key-value
  string parameter pairs are passed to the function as an array of
  `varjo_InterPupillaryDistanceParameters` structures. The supported parameters
  are "AdjustmentMode" (parameter value could be "Manual" or "Automatic") and
  "RequestedPositionInMM" (floating point IPD millimeters value, e.g. "64.5").
- Added new property `varjo_PropertyKey_IPDPosition` for getting the current
  headset IPD position in millimeters.
- Added new property `varjo_PropertyKey_IPDAdjustmentMode` for getting the
  currently used IPD adjustment mode.
- Added new function `varjo_ConvertToUnixTime()` which allows the conversion of
  Varjo monotonic timestamps to real time clock timestamps in nanoseconds since
  the Unix Epoch. This conversion is supported for up to one hour in past Varjo
  timestamps via periodic Varjo to real time clock synchronization.
- Added new constants `varjo_Error_InvalidParameter` and
  `varjo_Error_UnsupportedParameter` as new names for the existing error codes
  `varjo_Error_GazeInvalidParameter` and `varjo_Error_GazeUnsupportedParameter`
  in order to extend the usage scope for these error codes.

### Changed

- Modified the GazeTrackingExample application to support the new IPD control
  API. The example application now also uses the new `varjo_ConvertToUnixTime()`
  function to demonstrate the usage of time conversion from the Varjo timestamps
  to the real time clock (time-of-day) timestamps. The clock synchronization
  example code was removed as redundant after making use of the new time
  conversion API.

### Deprecated

- Deprecated the error code constants `varjo_Error_GazeInvalidParameter` and
  `varjo_Error_GazeUnsupportedParameter` in favor of the new more generically
  named constants `varjo_Error_InvalidParameter` and
  `varjo_Error_UnsupportedParameter` with the same error code values.

## 3.7.0

### Added

- Vulkan support. New header `Varjo_vk.h` and `varjo_VKCreateSwapChain()`
  function for Vulkan swapchain creation in `Varjo_layers.h`. New
  `varjo_RenderAPI` flag `varjo_RenderAPI_Vulkan`.
- Vulkan renderer for Benchmark example. Enable with `--renderer=vulkan`.
- Added new cubemap mode to allow cubemap color temperature and brightness to
  be adapted automatically to match the VST color frame. Mode can be changed
  with `varjo_MRSetEnvironmentCubemapConfig()`.
- Added new lock type `varjo_LockType_EnvironmentCubemap` to control access
  to the cubemap configuration.
- Added new metadata to the environment cubemap stream to simplify VR color
  correction. Instead of subscribing to distorted color stream, white balance
  and brightness normalization metadata can be accessed directly from the
  cubemap stream.
- Added eye openness ratio fields to the `varjo_EyeMeasurements` structure.
- Added `varjo_CancelGazeCalibration` function for cancelling gaze calibration.
  This function can be used to terminate active gaze calibration procedure, but
  also for just resetting calibration state to default. Note that depending on
  active "Foveated Rendering" option this either means that gaze tracking falls
  back to using "Best Estimation without Calibration" or stops outputting
  estimates for gaze direction. See documentation for further details.
- Added `varjo_GazeCalibrationParametersKey_HeadsetAlignmentGuidanceMode` option
  to control whether gaze calibration user interface waits for input from user
  (headset button press) before starting calibration sequence.
- Added new Blend Control Mask modes for depth occlusion masking.
- Added a new event type `varjo_EventType_MRChromaKeyConfigChange` which is
  triggered when the chroma key configuration has changed outside of the
  application control.
- Added a new camera property `varjo_CameraPropertyType_EyeReprojection` to
  control eye reprojection feature. This was previously part of the experimental
  SDK. AUTO mode offers eye reprojection according to the depth map, while
  MANUAL mode offers a new manual eye reprojection algorithm, where VST is
  reprojected according to a configurable static distance.

### Changed

- Modified GazeTrackingExample to support added gaze calibration API
  functionality and new eye openness data.
- Added a commandline option to EyeCameraStreamExample to run in a mode that
  streams all possible eye camera video frames from the runtime and prints
  FPS statistics for the stream. Default option is still to run application
  with UI and that is limited to (desktop) display refresh rate (VSYNC).

## 3.6.0

### Added

- Added `varjo_SetMirrorConfig()` and `varjo_ResetMirrorConfig()` functions
  to allow application access to composited image.
- Added `varjo_MirrorView` stucture which is used by `varjo_SetMirrorConfig()`
  function to specify views and and places where they should be rendered in
  mirror swapchain.
- Added support for triggering "One dot" gaze tracking calibration
  (see `varjo_GazeCalibrationParametersValue_CalibrationOneDot`)
- Added example how to convert Varjo timestamps to local time to
  `GazeTrackingExample`
- Added support for streaming eye camera feed using data stream API
  (`Varjo_datastream.h`). This camera feed will be available after gaze
  system has adjusted itself for a new user and is ready for tracking.
- Added new example, EyeCameraStreamExample, to show how to stream eye
  camera feed using previously mentioned API.
- Added `varjo_TextureFormat_Y8_UNORM` texture format. This format is used to
  transfer infrared eye camera stream and is not generally supported by
  rendering API.

### Changed

- MRExample now has option to undistort the VST color frame. This is just an
  example implementation on CPU. VST color stream is visualized in the scene
  so that user can see how undistort option affects to it.


## 3.5.0

### Added

- Added `varjo_EyeMeasurements` structure, which provides gaze tracker's
  estimates for user's pupil and iris diamaters in millimeters, their ratios
  and user's interpupillary distance.
- Added `varjo_GetGazeData()` and `varjo_GetGazeDataArray()` functions
  for retrieving `varjo_EyeMeasurements` data together with `varjo_Gaze` data.
- varjo_MRSetChromaKeyGlobal() function added to public MR API.

### Deprecated

- Deprecated `leftPupilSize` and `rightPupilSize` fields of `varjo_Gaze`
  structure. Change new code to use values provided in`varjo_EyeMeasurements`
  structure.


## 3.4.0

### Added

- `varjo_TextureFormat_B8G8R8A8_UNORM` format support. Developers should not
  generally utilize this format for RGB visual buffers as visible banding will
  be observed.
- Added new example, GazeTrackingExample, to show how gaze data and state can
  be accessed and logged directly from a command line application.

### Changed

- Rename `varjo_EventType_HeadsetStandbyStatus` to `varjo_EventType_StandbyStatus`.
  This better indicates that the event is raised when the whole Varjo system is in
  standby (i.e. not consuming application-generated frames at all).


## 3.3.0

### Added

- BlendControlMask view extension added to Layers API. This was previously
  part of Experimental SDK. MaskingTool example utilizing this API is now
  also available in public SDK. It still has depth test related experimental
  features that are only available when the example is built from Experimental
  SDK.

### Changed

- Documentation of `varjo_WorldObjectMarkerFlags` has been updated
- Renamed `varjo_TextureSize_Type_Recommended` to `varjo_TextureSize_Type_Quad`
  by deprecating the former option. This now better clarifies the usage intent
  for static fixed-foveated rendering.

### Deprecated

- `varjo_TextureSize_Type_BestQuality` resolution option. This is now synonymous
  with `varjo_TextureSize_Quad`.


## 3.2.0

### Added

- Property key `varjo_PropertyKey_UserPresence` for querying whether user is
  wearing the headset
- Benchmark example reacts to `varjo_EventType_TextureSizeChange` by creating
  new swapchains
- New error `varjo_Error_InvalidVariableRateShadingRadius` is given if
  a radius in `varjo_VariableRateShadingConfig` is not valid

### Changed

- Benchmark example project has smaller scale so it's easier to move around


## 3.1.0

### Added

- `varjo_D3D11UpdateVariableRateShadingTexture` and
  `varjo_GLUpdateVariableRateShadingTexture` for generating variable rate
  shading masks for D3D11 and OpenGL. Previously variable rate shading was
  only available for D3D12.
- The D3D11 and OpenGL variable rate shading functions require a
  `varjo_ShadingRateTable` that holds information about `varjo_ShadingRate`.
- Add example usage of variable rate shading for D3D11 and OpenGL to SDK example code
- Add `--fps` option to Benchmark example to output frames per second

### Changed

- Refactor common code for variable rate shading in example code
- ImGui UI stuff moved from experimental to regular SDK
- MRExample has ImGui based UI now
- Examples common code refactored for UI changes
- LayerView is now MultiLayerView and supports multiple layers
- Minor fix for `varjo_UpdateNearFarPlanes`


## 3.0.0

This release adds API functionality for new XR-3/VR-3 headsets.

### Added

- Foveated rendering API (finalized from earlier experimental version).
  This adds the following new functionality:
  - Ability to query FOV tangents for both foveated and non-foveated
    views (`varjo_GetFoveatedFovTangents` and `varjo_GetFovTangents`)
  - Function to query texture sizes (`varjo_GetTextureSize`) for best,
    recommended and foveated quality. Applications should migrate to using
    this API for fetching texture sizes.
  - `varjo_D3D12UpdateVariableRateShadingTexture` for generating various
    variable rate shading masks with D3D12 (other API support coming later).
- Improved gaze data API that supports parallel data streams and 200Hz
  gaze tracking.
- Helper function `varjo_GetProjectionMatrix` to convert FOV half tangents
  to projection matrix.
- `varjo_Error_SwapChainTextureIsNotReleased` error code. This error will be
  raised if application forgets to call `varjo_ReleaseSwapChainImage` before
  `varjo_EndFrameWithLayers`.

### Changed

- Refactor render target handling in example code
- Refactor some code common to all examples (logging)
- Improve marker rendering in MarkerExample application
- Update license

### Removed

- Legacy rendering API that was marked deprecated


## 2.6.0

### Added

- Examples are compiled with C++17


## 2.4.0

### Added

- Benchmark example: Support for rendering left eye views and right
  eye views on different gpus in sli for opengl, d3d12 (`--use-sli`)
  Support for rendering all eye views on slave gpu (`--use-slave-gpu`)


## 2.3.0

### Added

- New mixed reality camera property `varjo_CameraPropertyType_Sharpness`
- Generic `varjo_Lock()` and `varjo_Unlock()` functions for exclusively
  locking MR features for the calling session. Related new error codes
  `varjo_Error_AlreadyLocked ` and `varjo_Error_NotLocked`.
- New function `varjo_GetPropertyString` to get a string property
- New function `varjo_GetPropertyStringSize` to get the size of a buffer
  for a string property
- New properties to get the product name (`varjo_PropertyKey_HMDProductName`)
  and serial number (`varjo_PropertyKey_HMDSerialNumber`) of the HMD
- `varjo_RenderAPI_D3D12` that was missing from 2.2 release
- New error codes for rendering data validation:
  - `varjo_Error_InvalidSwapChain`
  - `varjo_Error_D3D12CreateTextureFailed`
  - `varjo_Error_WrongSwapChainTextureFormat`
  - `varjo_Error_InvalidViewExtension`
- Benchmark example: occlusion mesh rendering for all APIs, and
  support for reversed depth (`--reverse-depth`)

### Changed

- Update examples to use the new lock API
- `varjo_GetSupportedTextureFormats` can now return `varjo_Error_InvalidRenderAPI`
- Refactor common example code: headless view and logging macros
- Stabilize Varjo World API (beta in 2.2). This is a breaking change;
  apps built on top of 2.2 VarjoLib will stop working gracefully (i.e. markers
  will not be returned anymore with new 2.3 runtime). The main change is in
  naming as visual markers are now called object markers. Also, the separate
  dynamic and static marker types have been removed; prediction mode similar
  to 2.2 dynamic marker is available to get similar behavior.

### Deprecated

- Old MR feature specific lock/unlock functions deprecated


## 2.2.0

### Added

- DirectX 12 support. New header `Varjo_d3d12.h` and a separate function for
  DX12 swapchain creation in `Varjo_layers.h`.
- DirectX 12 renderer for Benchmark example. Enable with `--renderer=d3d12`.
- API for controlling chroma keying (`Varjo_mr.h` and `Varjo_mr_types.h`),
  a new feature in 2.2 software release
- New example, ChromaKeyExample, for showing the usage of chroma key feature
- Varjo world API (`Varjo_world.h` and `Varjo_types_world.h`). This API allows
  applications to query objects exposed by Varjo stack and the related components
  (such as the pose of an object). This API is currently used for visual markers,
  a new feature in 2.2 software release.
- New example, MarkerExample, for showing the basic usage of Varjo World API and
  visual marker feature

### Changed

- Move reusable classes from `MRExample` to `Common` folder
- Wrap all example classes with `VarjoExamples` namespace


## 2.1.0

### Added

- New error codes `varjo_Error_ValidationFailure`, `varjo_Error_InvalidSwapChainRect`
  and `varjo_Error_ViewDepthExtensionValidationFailure`. These tie with the
  added validation for `varjo_SubmitInfoLayers`.
- Benchmark example app commandline parameter `--depth-format` for setting
  the depth-stencil texture format (d32|d24s8|d32s8)
- View extension (`varjo_ViewExtensionDepthTestRange`) to define ranges for
  which the depth test is active. Outside the given range the layer is alpha-blended
  in layer order without depth testing.
- `varjo_UpdateNearFarPlanes` to replace deprecated `varjo_UpdateClipPlaneDistances`.
  The implementation of this function is in `Varjo_math.h`.

### Changed

- Improve MRExample application
- Update Benchmark to use `varjo_UpdateNearFarPlanes`

### Deprecated

- Legacy rendering API. This includes types in `Varjo_types.h` and
  functions in `Varjo.h`, `Varjo_d3d11.h` and `Varjo_gl.h`. All applications
  need to migrate to Layer API (defined in `Varjo_layers.h`).


## 2.0.0

The main addition in this release is the API to control Varjo mixed
reality devices and the related data streams.

### Added

- Mixed reality API (`Varjo_mr.h` and `Varjo_mr_types.h`) for controlling
  mixed reality devices and cameras
- Mixed reality API related events:
  - `varjo_EventType_MRDeviceStatus`
  - `varjo_EventType_MRCameraPropertyChange`
- Mixed reality API related error codes:
  - `varjo_Error_RequestFailed`
  - `varjo_Error_OperationFailed`
  - `varjo_Error_NotAvailable`
  - `varjo_Error_CapabilityNotAvailable`
  - `varjo_Error_CameraAlreadyLocked`
  - `varjo_Error_CameraNotLocked`
  - `varjo_Error_CameraInvalidPropertyType`
  - `varjo_Error_CameraInvalidPropertyValue`
  - `varjo_Error_CameraInvalidPropertyMode`
- Property key for querying whether a mixed reality capable device is
  currently connected (`varjo_PropertyKey_MRAvailable`)
- Data stream API (`Varjo_datastream.h` and `Varjo_datastream_types.h`) for
  subscribing to color camera and lighting cubemap streams
- Data stream API related events:
  - `varjo_EventType_DataStreamStart`
  - `varjo_EventType_DataStreamStop`
- Data stream API related error codes:
  - `varjo_Error_DataStreamInvalidCallback`
  - `varjo_Error_DataStreamInvalidId`
  - `varjo_Error_DataStreamAlreadyInUse`
  - `varjo_Error_DataStreamNotInUse`
  - `varjo_Error_DataStreamBufferInvalidId`
  - `varjo_Error_DataStreamBufferAlreadyLocked`
  - `varjo_Error_DataStreamBufferNotLocked`
  - `varjo_Error_DataStreamFrameExpired`
  - `varjo_Error_DataStreamDataNotAvailable`
- `varjo_BeginFrameWithLayers` to begin a frame when using the layer
  rendering API. This is effectively the same `varjo_BeginFrame` but
  without the second parameter.
- New parameters for controlling gaze output filter
- Mixed reality example (`MRExample`)
- New options for the benchmark example to run with video see-through

### Removed

- Flag `varjo_LayerFlag_BlendMode_Inherit`. Inheriting has no effect
  anymore since runtime version 2.0, so the flag is obsolete.
- Flag `varjo_LayerFlag_BlendMode_Opaque`. Opaque is the default blending
  mode since runtime version 2.0, so the flag is obsolete. Applications
  utilizing alpha blending need to turn it on using
  `varjo_LayerFlag_BlendMode_AlphaBlend`.

### Changed

- `varjo_LayerMultiProj::views` can be filled with just two views for
  submission of a stereo pair. Varjo compositor will split the image for
  the quad display devices. This is a runtime change, but important enough
  to be documented here as well.
- Rename `varjo_LayerFlag_DepthSorting` to `varjo_LayerFlag_DepthTesting`.
- Examples are now built using CMake. Instructions under `examples/README.txt`.

### Deprecated

- Blending / depth testing related submission flags (`varjo_SubmitFlag_Opaque`,
  `varjo_SubmitFlag_InvertAlpha` and `varjo_SubmitFlag_DepthSorting`). Behavior
  is controlled now for each layer separately.
- `varjo_SubmitInfoLayers::flags`. This field has no effect.


## 1.4.0

### Added

- Experimental layer rendering API (`Varjo_layers.h` and `Varjo_layer_types.h`).
    - NOTE: This will be finalized in the next release and might still be subject
      to small changes. The supplied example uses both rendering APIs. Developer
      documentation will be added in the next release as well.
- `varjo_RequestGazeCalibrationWithParameters` to control the gaze calibration
- `varjo_GetViewCount` to query number of views
- `varjo_Error_GazeInvalidParameter` to denote error of passing invalid
  parameters to `varjo_RequestGazeCalibrationWithParameters`
- `varjo_Error_D3D11CreateTextureFailed`
- Properties for querying gaze calibration quality per eye
  (`varjo_PropertyKey_GazeEyeCalibrationQuality_Left` and
   `varjo_PropertyKey_GazeEyeCalibrationQuality_Right`)
- Property for HMD connection status (`varjo_PropertyKey_HMDConnected`)
- New texture formats `A8_UNORM`, `YUV422`, `RGBA16_FLOAT`, `D24_UNORM_S8_UINT`
  and `D32_FLOAT_S8_UINT`

## 1.3.0

- Add `varjo_EventType_Foreground`
- Improve documentation

## 1.2.0

- Add support for 32-bit applications
- Improve examples
- Native SDK projection matrix is now off-axis to provide improved image quality
    – NOTE: Applications that don’t support off-axis projection should call
      `varjo_SetCenteredProjection(...)` function to enable old behavior.
    – NOTE: Applications built with <1.1 behave as previously.
- Separate error messages for rendering and gaze
- VarjoLib does not require CPU with AVX support anymore
