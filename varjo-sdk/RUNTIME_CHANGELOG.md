# Varjo Runtime Changelog

This document lists all changes between the Varjo software stack releases
that affect the expected application behavior (common to apps built on various
SDK versions). CHANGELOG.md documents the changes on the SDK side and any
new supported functionality.

## 4.1

### Changed

- Changed gaze tracking initialization to default to use maximum tracking frequency
  supported by the connected headset. Previously runtime defaulted to use 100Hz
  tracking and if application didn't explicitly set tracking frequency, it might
  have resulted in running gaze tracking at reduced frame rate unintentionally.

  NOTE: Some applications that expect a reduced tracking rate might require code fix
  due to this change. In a such case application should explicitly set output
  frequency parameter to initialize 100Hz tracking or modify the application to
  support higher tracking frequencies.

## 3.10.0

Removed:
- Removed legacy rendering API functions: `varjo_BeginFrame`, `varjo_EndFrame`,
`varjo_CreateSubmitInfo`, `varjo_FreeSubmitInfo`,`varjo_GetCurrentSwapChainConfig`,
`varjo_GetSwapChainCurrentIndex`, `varjo_UpdateClipPlaneDistances`
`varjo_LayoutDefaultViewports`, `varjo_D3D11Init`, `varjo_D3D11Shutdown`,
`varjo_GLInit`, `varjo_GLShutdown`
- Removed support for Legacy 10-dot gaze calibration. Newer Fast 5-dot gaze
  calibration allows to achieve higher accuracy and precision. Requesting Legacy
  10-dot gaze calibration will trigger Fast 5-dot calibration.

## 3.8.0

Fixed:
- `varjo_GetVersionString`, `varjo_GetVersion`, `varjo_GetFullVersion` were
  modified to return runtime version (installed VarjoBase version) instead of
  the SDK version the client app was linked against.

## 3.7.0

Changed:
- When forced VST depth testing enabled, depth test flag was before also applied
  to blend control mask layers, but not anymore. This didn't work with the new
  masking modes for depth testing.

Fixed:
- Resetting camera properties will now raise an event
  `varjo_EventType_MRCameraPropertyChange` if the mode or value is affected by
  the reset.

## 3.4.0

Changed:
- `varjo_GazeInit` and `varjo_GazeInitWithParameters` can now succeed without
  connected headset. In such case, without headset, `varjo_GetGaze`,
  `varjo_GetGazeArray` and `varjo_GetRenderingGaze` won't raise
  `varjo_Error_GazeNotInitialized` nor `varjo_Error_GazeNotConnected` errors,
  however returned `varjo_Gaze` will still be invalid (will have `status`
  member set to `varjo_GazeStatus_Invalid`) until user connects and aligns
  headset to eyes. This change allows for simpler Varjo gaze initialization
  calls on application side, not requiring waiting for a headset connection,
  and reduces gaze errors logging when running and debugging application
  without connected headset.

Fixed:
- Fixed runtime to not skip frames under some specific conditions.
  If the client rendered frames with CPU frame time around 9-11ms (for 90hz)
  with comparatively lesser GPU frame time, then there were occurrences of
  runtime sleeping too long in `varjo_WaitSync`.


## 3.3.0

Changed:
- `varjo_TextureSize_BestQuality` will now return the same value as for
  `varjo_TextureSize_Recommended`, and both honor the user-selectable
  resolutions setting from Varjo Base.
- In general, all resolution values returned by any integration API now
  change based on user-selected quality level in Varjo Base. This applies
  to all rendering modes (stereo, static quad, foveation with dynamic
  viewports).
- Compositor will now downlock application FPS to some multiple of target
  HW refresh rate when VSYNC is turned on in Varjo Base.
- Application frames will go under extra warping procedure when "Motion "
  Prediction" from Varjo Base is turned to "Enabled if supported" mode
  when depth is submitted.

Fixed:
- `varjo_PropertyKey_GazeCalibrating` is set to `varjo_True` already when
  headset alignment guidance becomes active.


## 3.2.0

Changed:
- `varjo_GetRenderingGaze` still returns minimum information required for
  foveation even if "Allow eye tracking" - a setting in Varjo Base - is in OFF
  state.
- Runtime will switch automatically to synchronized frame submission under
  low FPS scenarios to reduce display latency.
- Composition time image is warped adatively based on FPS. This increases
  quality of the final composited image under low FPS scenarios, but causes
  some additional performance overhead.


## 3.1.1

Changed:
- Video-pass-through default depth test range has been changed from infinity
  to 0.75m. As a result, depth test is applied only at close range, and beyond
  that VR content is always shown. Applications can override this default by
  using varjo_ViewExtensionDepthTestRange layer extension. In case any
  application has set explicit depth test range by providing this extension,
  the default range for video-see-through is not applied, but the range from
  application is to be used.


## 3.1.0

Added:
- A new event type `varjo_EventType_TextureSizeChange` is triggered when a
  return value of `varjo_GetTextureSize` is changed. The event holds information
  of what type of texture has a its size changed in `varjo_TextureSize_Type_Mask`.
- A new error `varjo_Error_UnableToInitializeVariableRateShading` is set if the
  variable rate shading can not be initialized on runtime.

Changed:
- `varjo_GetRenderingGaze` is disabled if application FPS is too low to avoid
  using dynamic viewport foveated rendering with low FPS.
- Camera property change event won't be sent anymore if the same mode or value was already set.

Fixed:
- `varjo_GetRenderingGaze` respects the "Allow eye tracking" -setting from Varjo Base


## 3.0.0

Support for XR-3/VR-3.


## 2.5.0

Changed:
- `varjo_EndFrameWithLayers` will raise an error (`varjo_Error_SwapChainTextureIsNotReleased`)
  if one of the used swapchain wasn't relased.


## 2.3.0

Fixed:
- Fix a bug that `varjo_SyncProperties` had to be manually called at least
  once before `varjo_GetProperty*` functions returned valid property values
- Add support for D24S8 and D32S8 texture formats for DirectX 12 rendering
  path
- Improve swapchain validation (related to the new error codes)

Removed:
- Support for Varjo World visual markers. See CHANGELOG.md for more information
  about the new object marker API.


## 2.2.0

Fixed:
- Fix a bug in `varjo_StreamFrame::hmdPose`: the value was HMD center view
  matrix instead of world pose. If you need the old functionality, invert
  the value to get the view matrix.

Known issues:
- DirectX 12 rendering path does not support D24S8 and D32S8 depth formats.
  D32 is supported.


## 2.1.0

### Rendering

Changed:

- Improve swapchain config validation. This validation is only enabled if the
  application is built on 2.1 SDK or newer.
- Improve validation of `varjo_SubmitInfoLayers`. New error codes were
  added (see `CHANGELOG.md`). This validation is only enabled if the application
  is built on 2.1 SDK or newer.
- Implement various performance improvements
- Add occlusion mesh support for OpenVR

Fixed:

- Fix tearing and general choppiness of presented frames after screen recording
  was activated and deactivated. Fixing this previously required a restart
  of Varjo stack.
- Fix OpenGL depth decode, which resulted in wrong positional timewarp and
  depth testing behavior
- Fix Vsync behavior when video pass-through is active

### Pose tracking

Added:

- Added 3DoF tracking fallback in case the 6DoF tracking becomes unavailable.

Fixed:

- Usage of tracking plugin verifier tool to set active tracking plugin could
  previously cause rendering performance issues.

### Mixed Reality

Changed:

- Allow calls to MR API when MR capable device is not connected to make it
  possible to stop features on disconnected callback. Starting MR features
  is not allowed when disconnected.
- Improve quality of depth estimation


## 2.0.0

### Rendering

Changed:

- Increase the default occlusion mesh scaling from 1.0 to 1.3 to give
  timewarp some breathing room. Effectively, `varjo_CreateOcclusionMesh`
  returns a similar mesh as before, but with slight expansion.
- Introduce radial clamping to the occlusion mesh edges (i.e. clamp sample
  on the border instead of sampling the occlusion mesh contents). This will
  be enabled if applications use the layer rendering API and pass the
  `varjo_LayerFlag_UsingOcclusionMesh` flag. This flag also ensures that
  6-DOF timewarp is not applied within the occlusion mesh area.
- Force legacy rendering API (defined in `Varjo.h`) to always default to
  opaque rendering to make existing VR applications compatible with features
  introduced in Varjo Base 2.0. Applications which need alpha blending must
  transition to the layer rendering API.
- Change the default blending mode of the layer API to opaque. Applications
  must explicitly pass `varjo_LayerFlag_BlendMode_AlphaBlend` to enable
  (premultiplied) alpha blending.
- Increase maximum supported blended layer count from 4 to 6.
- `varjo_LayerMultiProj::views` can be now filled with just two views for
  stereo rendering. Runtime will automatically split the given views if the
  device has more displays than the provided view count.
- Far plane distance (`farZ`) in `varjo_ViewExtensionDepth` can now be assigned
  to the IEEE-compliant value of double-precision +infinity. This effectively
  sets the far clip plane of the frustum to infinity.

Fixed:

- Fix depth layer extension (`varjo_ViewExtensionDepth`) to properly use the
  values provided via nearZ and farZ parameters, which must be assigned to the
  near and far clipping distances correspondingly. For reversed depth buffer the
  values of nearZ and farZ must be swapped (i.e. nearZ receives the distance to
  the far clip plane and farZ receives the distance to the near clip plane).
- Fix compositor to stop rendering layers that application does not submit on
  the subsequent frames.
- Fix `varjo_GetViewDescription()` to not require graphics initialization. Correct
  values will be returned immediately after a session has been created.

### Eye tracking

Changed:

- Default gaze calibration (invoked by calling `varjo_RequestGazeCalibration`)
  model has changed to the new Fast model which halves the calibration duration
  and increases robustness to HMD shifts. The old 10 dot calibration sequence
  remains as an API option behind the `varjo_RequestGazeCalibrationWithParameters`
  extension (and may still be a better choice for certain medical conditions).
- Amend reported quality metrics algorithm for Fast calibration based on findings
  from large scale trials.

### Pose tracking

Added:

- Support for Tracking Plugin API which enables support of other HMD pose tracking
  solutions besides Steam.

### Mixed reality

Added:

- Initial support for mixed reality devices
