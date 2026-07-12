/*
 * vr_menu_quad.h — world-anchored 2D-UI quad for VR (Milestone 3, HUD/menus).
 *
 * Replaces the Android fork's OpenXR XrCompositionLayerQuad "big screen"
 * (TBXR_Common.c:2131-2157): whenever the engine is in 2D-UI mode (menu open,
 * console up, demo playback, connecting/loading — the same VR_UseScreenLayer()
 * predicate the fork used, QuakeQuest_OpenXR.c:97-100), the frame is rendered
 * ONCE, flat, into an offscreen FBO texture, and that texture is drawn per eye
 * as a textured quad fixed in the WORLD (anchored at the head pose captured
 * when 2D-UI mode was entered — re-anchored on each open, never per-frame, so
 * it does not head-lock). WebXR has no reliable composition-layer equivalent
 * (reports/06 §3.2), hence the in-scene quad.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef VR_MENU_QUAD_H_
#define VR_MENU_QUAD_H_

#include <stdbool.h>

#include "lib/webxr/webxr.h" /* WebXRView */

/* The fork's host-side big-screen state (QuakeQuest_OpenXR.c:95,107-113).
 * The ENGINE drives it: menu.c/console.c/cl_screen.c call BigScreenMode(1/0)
 * on menu open/close and console show/hide (mode 2 = locked, boot credits). */
extern int bigScreen;
void BigScreenMode(int mode); /* engine declares this extern itself */

/* Register the vr_menu_distance / vr_menu_width cvars (after Host_Main). */
void VRMenuQuad_Init(void);

/* Run one XR frame in screen-layer mode if the engine is in 2D-UI mode:
 * renders the flat frame into the menu FBO and draws the world-anchored quad
 * into both eyes' viewports of the XR layer framebuffer. Called from the
 * bridge's OnXRFrame INSTEAD of the stereo pump.
 *
 * Returns true if it handled the frame (bridge must skip the stereo path),
 * false when not in 2D-UI mode (or the FBO isn't available — graceful
 * fallback to the old head-locked stereo render). */
bool VRMenuQuad_RunFrame(const WebXRView views[2]);

/* Probe for tests (EMSCRIPTEN_KEEPALIVE): bit 0 = quad path active this
 * frame, bits 4..7 = bigScreen, bit 8 = console active, bit 9 = demoplayback,
 * bits 12..15 = key_dest, bits 16..23 = m_state. */
int VRMenuQuad_DebugState(void);

#endif
