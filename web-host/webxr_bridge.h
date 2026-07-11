/*
 * webxr_bridge.h — WebXR session/rendering bridge (Milestone 2).
 *
 * Implements reports/05-webxr-bridge-design.md §6: session lifecycle,
 * flatscreen-rAF <-> XR-rAF handoff, per-eye stereo rendering into the
 * XRWebGLLayer framebuffer, and head-pose -> engine plumbing. Owns the
 * VR_GetVRProjection / VR_SetHMDOrientation / VR_SetHMDPosition symbols
 * (single source of truth — main_web.c must NOT define these).
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef WEBXR_BRIDGE_H_
#define WEBXR_BRIDGE_H_

#include <stdbool.h>

/* One-time setup: registers the webxr_init() callbacks. Call from main()
 * after Host_Main() (engine + GL context up), before the flatscreen loop. */
void WebXRBridge_Init(void);

/* Exported to JS (EMSCRIPTEN_KEEPALIVE) for the HTML shell's "Enter VR"
 * button. MUST be called synchronously from a user-activation event. */
void WebXRBridge_RequestSession(void);

/* Exported to JS for an "Exit VR" affordance. */
void WebXRBridge_RequestExit(void);

/* True while an immersive session is active. Gates VR_UseScreenLayer()
 * and any flatscreen resize handling. */
bool WebXRBridge_IsSessionActive(void);

/* Vertical FOV (degrees) derived from the current XR projection matrix,
 * or 0 when no XR frame data is available (caller falls back to flat 90). */
float WebXRBridge_GetFOV(void);

/* Ported, de-OpenXR-ified copy of QuakeQuestSrc/TBXR_Common.c:911-938's
 * coordinate remap (design doc §4.2). q = [x,y,z,w] WebXR/OpenXR-space
 * quaternion; out = [pitch,yaw,roll] Quake degrees. rotationAdjust is the
 * optional extra rotation the original applied (pass NULL for none). */
void WebXRBridge_QuatToYawPitchRoll(const float q[4], const float rotationAdjust[3], float out[3]);

#endif
