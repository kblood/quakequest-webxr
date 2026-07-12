#ifndef WEBXR_H_
#define WEBXR_H_

/** @file
 * @brief Minimal WebXR Device API wrapper
 *
 * Vendored from emscripten-webxr @ 1bc0b7b (MIT — see COPYING) for the
 * QuakeQuest->WebXR port. Deviations from upstream are marked
 * "WEBXR-PORT PATCH #n" and documented in PATCHES.md.
 *
 * WEBXR-PORT PATCH #10: upstream's header only compiled as C++ —
 *  - the extern "C" guard was malformed (the opening `{` was emitted even in
 *    plain C, a file-scope syntax error, and there was no closing guard),
 *  - enums were used as type names without typedefs (C++ only),
 *  - webxr_get_input_pose declared a C++ default argument.
 * All three fixed below; no functional change for C++ consumers.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Errors enum */
typedef enum WebXRError {
    WEBXR_ERR_API_UNSUPPORTED = -2, /**< WebXR Device API not supported in this browser */
    WEBXR_ERR_GL_INCAPABLE = -3, /**< GL context cannot render WebXR */
    WEBXR_ERR_SESSION_UNSUPPORTED = -4, /**< given session mode not supported */
} WebXRError;

/** WebXR handedness */
typedef enum WebXRHandedness {
    WEBXR_HANDEDNESS_NONE = -1,
    WEBXR_HANDEDNESS_LEFT = 0,
    WEBXR_HANDEDNESS_RIGHT = 1,
} WebXRHandedness;

/** WebXR target ray mode */
typedef enum WebXRTargetRayMode {
    WEBXR_TARGET_RAY_MODE_GAZE = 0,
    WEBXR_TARGET_RAY_MODE_TRACKED_POINTER = 1,
    WEBXR_TARGET_RAY_MODE_SCREEN = 2,
} WebXRTargetRayMode;

/** WebXR 'XRSessionMode' enum*/
typedef enum WebXRSessionMode {
    WEBXR_SESSION_MODE_INLINE = 0, /** "inline" */
    WEBXR_SESSION_MODE_IMMERSIVE_VR = 1, /** "immersive-vr" */
    WEBXR_SESSION_MODE_IMMERSIVE_AR = 2, /** "immersive-ar" */
} WebXRSessionMode;

/** WebXR session feature bits */
typedef enum WebXRSessionFeatures {
    WEBXR_SESSION_FEATURE_LOCAL = 1, /** "local" (1 << 0) */
    WEBXR_SESSION_FEATURE_LOCAL_FLOOR = 2, /** "local-floor" (1 << 1) */
    WEBXR_SESSION_FEATURE_BOUNDED_FLOOR = 4, /** "bounded-floor" (1 << 2) */
    WEBXR_SESSION_FEATURE_UNBOUNDED = 8, /** "unbounded" (1 << 3) */
    WEBXR_SESSION_FEATURE_HIT_TEST = 16, /** "hit-test" (1 << 4) */
} WebXRSessionFeatures;
/* WEBXR-PORT note: upstream declared these as ordinal indices (0,1,2,3,4)
 * although both library_webxr.js and the upstream README treat the value as a
 * BITMASK (1 << i). Requesting LOCAL_FLOOR (1) under the ordinal scheme
 * actually requested "local". Values fixed to the bit the JS tests for. */

/** WebXR input pose mode */
typedef enum WebXRInputPoseMode {
    WEBXR_INPUT_POSE_GRIP = 0, /** gripSpace */
    WEBXR_INPUT_POSE_TARGET_RAY = 1, /** targetRaySpace */
} WebXRInputPoseMode;

/** WebXR rigid transform */
typedef struct WebXRRigidTransform {
    float matrix[16];
    float position[3];
    float orientation[4];
} WebXRRigidTransform;

/** WebXR view */
typedef struct WebXRView {
    /* view pose */
    WebXRRigidTransform viewPose;
    /* projection matrix */
    float projectionMatrix[16];
    /* x, y, width, height of the eye viewport on target texture */
    int viewport[4];
} WebXRView;

typedef struct WebXRInputSource {
    int id;
    WebXRHandedness handedness;
    WebXRTargetRayMode targetRayMode;
} WebXRInputSource;

/* WEBXR-PORT PATCH #12: full per-frame controller snapshot (poses + gamepad).
 * Upstream only exposed pose queries (webxr_get_input_pose) and select events;
 * the Gamepad API surface (buttons/axes/haptics presence) was unreachable from
 * C. Layout is marshaled field-by-field in library_webxr.js — keep the two in
 * sync (all members are 4-byte scalars; struct size = 196 bytes). */
#define WEBXR_INPUT_MAX_BUTTONS 8
#define WEBXR_INPUT_MAX_AXES 4

typedef struct WebXRButtonState {
    int pressed;    /* GamepadButton.pressed (0/1) */
    int touched;    /* GamepadButton.touched (0/1) */
    float value;    /* GamepadButton.value (0..1, analog for trigger/squeeze) */
} WebXRButtonState;

typedef struct WebXRControllerState {
    int present;                 /* an XRInputSource with this handedness exists */
    int gripValid;               /* gripSpace pose located this frame */
    float gripPosition[3];       /* meters, XR reference space (x right, y up, z back) */
    float gripOrientation[4];    /* quaternion x,y,z,w */
    int aimValid;                /* targetRaySpace pose located this frame */
    float aimPosition[3];
    float aimOrientation[4];
    int gamepadConnected;        /* inputSource.gamepad != null ("xr-standard") */
    int hasHaptic;               /* gamepad.hapticActuators[0].pulse available */
    int buttonCount;             /* valid entries in buttons[] */
    int axisCount;               /* valid entries in axes[] */
    WebXRButtonState buttons[WEBXR_INPUT_MAX_BUTTONS];
    float axes[WEBXR_INPUT_MAX_AXES]; /* xr-standard: [2]=thumbstick x, [3]=thumbstick y (+y = DOWN) */
} WebXRControllerState;

/**
Callback for errors

@param userData User pointer passed to init_webxr()
@param error Error code
*/
typedef void (*webxr_error_callback_func)(void* userData, int error);

/**
Callback for frame rendering

@param userData User pointer passed to init_webxr()
@param time Current frame time
@param headPose Pose of the XR device relative to tracking origin
@param views Array of `viewCount` @ref WebXRView "webxr views"
@param viewCount Size of `views`
*/
typedef void (*webxr_frame_callback_func)(void* userData, int time, WebXRRigidTransform* headPose, WebXRView views[2], int viewCount);

/**
Callback for VR session start

@param userData User pointer passed to set_session_start_callback
@param mode The session mode
*/
typedef void (*webxr_session_callback_func)(void* userData, int mode);

/**
Callback for @ref webxr_is_session_supported

@param mode The session mode that was requested
@param supported Whether given mode is supported by this device
*/
typedef void (*webxr_session_supported_callback_func)(int mode, int supported);


/**
Init WebXR rendering

@param frameCallback Callback called every frame
@param sessionStartCallback Callback called when session is started
@param sessionEndCallback Callback called when session ended
@param errorCallback Callback called every frame
@param userData User data passed to the callbacks
*/
extern void webxr_init(
        webxr_frame_callback_func frameCallback,
        webxr_session_callback_func sessionStartCallback,
        webxr_session_callback_func sessionEndCallback,
        webxr_error_callback_func errorCallback,
        void* userData);

extern void webxr_set_session_blur_callback(
        webxr_session_callback_func sessionBlurCallback, void* userData);
extern void webxr_set_session_focus_callback(
        webxr_session_callback_func sessionFocusCallback, void* userData);


/*
Test if session mode is supported

@param mode Session mode to test
@param supportedCallback Callback which will be called once the
        result has become available
*/
extern void webxr_is_session_supported(WebXRSessionMode mode,
        webxr_session_supported_callback_func supportedCallback);
/*
Request session presentation start

@param mode Session mode from @ref WebXRSessionMode.
@param requiredFeatures Required session features from @ref WebXRSessionFeatures
@param optionalFeatures Optional session features from @ref WebXRSessionFeatures

Needs to be called from a [user activation event](https://html.spec.whatwg.org/multipage/interaction.html#triggered-by-user-activation).
*/
extern void webxr_request_session(WebXRSessionMode mode,
    WebXRSessionFeatures requiredFeatures,
    WebXRSessionFeatures optionalFeatures);

/*
Request that the webxr presentation exits VR mode
*/
extern void webxr_request_exit(void);

/**
Set projection matrix parameters for the webxr session

@param near Distance of near clipping plane
@param far Distance of far clipping plane
*/
extern void webxr_set_projection_params(float near, float far);

/**

WebXR Input

*/

/**
Callback for primary input action.

@param userData User pointer passed to @ref webxr_set_select_callback, @ref webxr_set_select_end_callback or @ref webxr_set_select_start_callback.
*/
typedef void (*webxr_input_callback_func)(WebXRInputSource* inputSource, void* userData);


/**
Set callbacks for primary input action.
*/
extern void webxr_set_select_callback(
        webxr_input_callback_func callback, void* userData);
extern void webxr_set_select_start_callback(
        webxr_input_callback_func callback, void* userData);
extern void webxr_set_select_end_callback(
        webxr_input_callback_func callback, void* userData);

/**
Get input sources.

@param outArray @ref WebXRInputSource array to fill.
@param max Size of outArray (in elements).
@param outCount Will receive the number of input sources valid in outArray.
*/
extern void webxr_get_input_sources(
        WebXRInputSource* outArray, int max, int* outCount);

/**
Get input pose. Can only be called during the frame callback.

@param source The source to get the pose for.
@param outPose Where to store the pose.
@param mode Which space to query (grip or target ray).
@returns `false` if updating the pose failed, `true` otherwise.
*/
extern int webxr_get_input_pose(WebXRInputSource* source, WebXRRigidTransform* outPose, WebXRInputPoseMode mode);

/* WEBXR-PORT PATCH #12 (see struct above).
Snapshot one hand's controller state (poses + gamepad buttons/axes).
Must be called during the frame callback (poses need the XRFrame).
`out` is always fully zeroed first, so absent hands read as all-zero.

@param handedness WEBXR_HANDEDNESS_LEFT or WEBXR_HANDEDNESS_RIGHT.
@param out Receives the state.
@returns 1 if an input source with that handedness exists, else 0. */
extern int webxr_get_controller_state(int handedness, WebXRControllerState* out);

/* WEBXR-PORT PATCH #13: fire a haptic pulse on one hand's controller via
gamepad.hapticActuators[0].pulse() (playEffect fallback). Safe no-op (returns
0) when there is no session, no such hand, or no actuator. Callable any time
(does not need the frame callback).

@param handedness WEBXR_HANDEDNESS_LEFT or WEBXR_HANDEDNESS_RIGHT.
@param intensity 0..1 (clamped).
@param durationMs pulse length in milliseconds.
@returns 1 if a pulse was issued, else 0. */
extern int webxr_haptic_pulse(int handedness, float intensity, int durationMs);

#ifdef __cplusplus
}
#endif

#endif
