/*
 * webxr_input.h — WebXR controller-input foundation (Milestone 3).
 *
 * The layer every M3 gameplay chunk (reports/06 §5: locomotion, weapon aim,
 * HUD/menus, comfort) consumes. Two views of the same per-frame data:
 *
 *  1. RAW (webxr_controllers[hand]) — exactly what the browser reports:
 *     grip pose + targetRay (aim) pose, xr-standard gamepad buttons
 *     (pressed/touched/value) and axes, presence/haptics flags. Nothing
 *     interpreted. See WebXRControllerState in lib/webxr/webxr.h.
 *
 *  2. TBXR-COMPATIBLE (leftTrackedRemoteState_new & friends) — the same
 *     globals, names, types and semantics that QuakeQuestSrc's
 *     TBXR_UpdateControllers() produced on Android (TBXR_Common.h:42-79,
 *     OpenXrInput.c:430-501), so HandleInput_Default()'s logic ports onto
 *     this file with minimal rewording:
 *       - Buttons/Touches are the same xrButton_* bitmask values;
 *       - IndexTrigger/GripTrigger are the analog values, and the bit is set
 *         at the same >0.5 threshold OpenXrInput.c used;
 *       - Joystick is OpenXR sign convention (+y = stick pushed FORWARD/UP).
 *         The xr-standard Gamepad convention is +y = down, the sign flip
 *         happens HERE — do not flip again in gameplay code;
 *       - *RemoteTracking_new.Pose is the AIM pose (the fork tracked
 *         controllers via the OpenXR aim space, reports/06 §1.1), raw XR
 *         reference-space meters (x right, y up, z back) — consumers do the
 *         XR->Quake remap themselves exactly as before. Grip pose is only in
 *         the raw view (the fork never used it);
 *       - Velocity.linearVelocity is DERIVED from aim-position deltas (the
 *         WebXR Gamepad path has no XrSpaceVelocity equivalent); same axes
 *         and m/s units as the OpenXR value it replaces.
 *
 * The foundation updates the *_new state once per XR frame (called from
 * webxr_bridge.c's OnXRFrame, before QC_BeginFrame). *_old is NOT touched
 * here: gameplay code saves new->old itself at the end of its per-hand
 * blocks, exactly like HandleInput_Default did (QuakeQuest_OpenXR.c:743-744,
 * 901, 1004). Outside a session everything reads as zero/inactive.
 *
 * Quest Touch xr-standard layout (reports/06 §1.4; re-verify indices on the
 * live Quest browser during M3 headset testing):
 *   buttons[0] trigger (analog)   buttons[1] squeeze/grip (analog)
 *   buttons[2] unused (touchpad slot)   buttons[3] thumbstick click
 *   buttons[4] A (right) / X (left)     buttons[5] B (right) / Y (left)
 *   buttons[6] thumbrest (touch only, may be absent)
 *   axes[0,1] unused (touchpad slot)    axes[2,3] thumbstick x,y (+y = down!)
 * GOTCHA: the Quest Menu button (fork's xrButton_Enter menu toggle) is
 * reserved by the browser and NEVER appears as a gamepad button — chunk 3
 * must rebind the menu toggle (reports/06 §1.4).
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef WEBXR_INPUT_H_
#define WEBXR_INPUT_H_

#include <stdbool.h>
#include <stdint.h>

#include "lib/webxr/webxr.h" /* WebXRControllerState (raw view) */

#define WEBXR_HAND_LEFT  0
#define WEBXR_HAND_RIGHT 1
#define WEBXR_HAND_COUNT 2

/* xrButton_* — same values as QuakeQuestSrc/TBXR_Common.h:42-65 so ported
 * gameplay code keeps its masks verbatim. Only the bits the web layer can
 * actually set are listed as settable; the rest exist for compatibility.
 * NOTE: xrButton_Enter (menu button) is never set in the browser. */
typedef enum xrButton_ {
    xrButton_A           = 0x00000001,
    xrButton_B           = 0x00000002,
    xrButton_RThumb      = 0x00000004,
    xrButton_RShoulder   = 0x00000008,
    xrButton_X           = 0x00000100,
    xrButton_Y           = 0x00000200,
    xrButton_LThumb      = 0x00000400,
    xrButton_LShoulder   = 0x00000800,
    xrButton_Up          = 0x00010000,
    xrButton_Down        = 0x00020000,
    xrButton_Left        = 0x00040000,
    xrButton_Right       = 0x00080000,
    xrButton_Enter       = 0x00100000, /* Menu button — NOT AVAILABLE in browsers */
    xrButton_Back        = 0x00200000,
    xrButton_GripTrigger = 0x04000000,
    xrButton_Trigger     = 0x20000000,
    xrButton_Joystick    = 0x80000000,
    xrButton_ThumbRest   = 0x00000010, /* touch only */
    xrButton_EnumSize    = 0x7fffffff
} xrButton;

/* Shapes mirror TBXR_Common.h's XrVector2f/ovrInputStateTrackedRemote/
 * ovrTrackedController field-for-field (minus OpenXR types) so member access
 * in ported code (.Joystick.x, ->Pose.position.y, ->Velocity.linearVelocity.z)
 * compiles unchanged. */
typedef struct { float x, y; }       WebXRVec2;
typedef struct { float x, y, z; }    WebXRVec3;
typedef struct { float x, y, z, w; } WebXRQuat;

typedef struct {
    uint32_t Buttons;    /* xrButton_* bitmask */
    uint32_t Touches;    /* xrButton_* bitmask (capacitive touch) */
    float IndexTrigger;  /* 0..1 analog */
    float GripTrigger;   /* 0..1 analog */
    WebXRVec2 Joystick;  /* deflection -1..1, OpenXR signs (+y = forward/up) */
} WebXRRemoteState;      /* == ovrInputStateTrackedRemote */

typedef struct {
    bool Active;         /* hand present AND aim pose located this frame */
    struct {
        WebXRVec3 position;    /* aim pose, XR ref-space meters */
        WebXRQuat orientation; /* aim pose quaternion x,y,z,w */
    } Pose;
    struct {
        WebXRVec3 linearVelocity; /* m/s, derived from position deltas */
    } Velocity;
} WebXRTrackedController; /* == ovrTrackedController (Pose/Velocity subset used) */

/* ---- per-frame state (foundation writes *_new each XR frame; gameplay
 * code owns the new->old copy, as in the fork) ---- */
extern WebXRControllerState webxr_controllers[WEBXR_HAND_COUNT]; /* raw view */

extern WebXRRemoteState leftTrackedRemoteState_new,  leftTrackedRemoteState_old;
extern WebXRRemoteState rightTrackedRemoteState_new, rightTrackedRemoteState_old;
extern WebXRTrackedController leftRemoteTracking_new, rightRemoteTracking_new;

/* ---- lifecycle (called by webxr_bridge.c / main_web.c) ---- */

/* One-time setup after Host_Main(): registers the vr_inputdebug console
 * command and honors ?inputdebug=1. */
void WebXRInput_Init(void);

/* Snapshot both hands + tick haptics + debug output. Called once per XR
 * frame from the bridge's OnXRFrame (inside the frame callback — poses need
 * the live XRFrame), before QC_BeginFrame. nowMs = emscripten_get_now(). */
void WebXRInput_Update(double nowMs);

/* Zero all input state (session ended — don't leak stale buttons/poses). */
void WebXRInput_Reset(void);

/* ---- haptics ---- */

/* Drop-in replacement for TBXR_Vibrate (OpenXrInput.c:508-525): same
 * signature and channel semantics, so VR_HapticEvent's ported per-weapon
 * table calls it unchanged.
 *   durationMs: pulse length in ms; -1 = continuous until cancelled;
 *               0 = cancel a continuous vibration.
 *   channelMask: 1 = left, 2 = right, 3 = both.
 *   intensity: 0..1.
 * Requests for a channel still mid-vibration are ignored (fork behavior).
 * No-op when the hand has no haptic actuator (e.g. emulated runtime). */
void WebXRInput_Vibrate(int durationMs, int channelMask, float intensity);

/* Raw single pulse on one hand, no channel bookkeeping (thin wrapper over
 * gamepad.hapticActuators[0].pulse). Returns false if no actuator. */
bool WebXRInput_HapticPulse(int hand, float intensity, int durationMs);

/* ---- helpers for the chunk agents ---- */

/* HMD position delta this frame (raw XR axes, meters) — the fork's
 * positionDeltaThisFrame (VR_SetHMDPosition, QuakeQuest_OpenXR.c:195-216),
 * needed by chunk 1 (positional locomotion) and chunk 4 (bullet-time). */
void WebXRInput_GetHMDPositionDelta(float out[3]);

/* Enable/disable the live input-state dump (DOM overlay + 1 Hz Con_Printf
 * notify lines, visible in-headset). Also reachable as the console command
 * `vr_inputdebug [0|1]` and the ?inputdebug=1 URL param. */
void WebXRInput_SetDebug(int enabled);

#endif
