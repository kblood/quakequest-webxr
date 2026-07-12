/*
 * in_locomotion.h — M3 chunk 1: VR locomotion & turning.
 *
 * Ports the locomotion half of QuakeQuestSrc/QuakeQuest_OpenXR.c's
 * HandleInput_Default() (reports/06-vr-gameplay-map.md §1.2/§5 Chunk 1) onto
 * the WebXR controller-input foundation (web-host/webxr_input.h). Reads:
 *   - thumbstick smooth locomotion (off-hand stick -> heading-rotated
 *     sideways/forward move, HandleInput_Default:936-951),
 *   - HMD positional-delta walking (right-hand block, :810-827),
 *   - the always-right-hand turn stick feeding QC_MotionEvent's
 *     snap/smooth-turn state machine (:834-835, vid_android.c QC_MotionEvent/
 *     IN_Move — already engine-side, unmodified, see PORT_NOTES),
 *   - the off-hand trigger -> +speed (run) / fire binding (:953-963).
 * Does NOT touch: dominant-hand weapon aim/gunangles (chunk 2), bigScreen
 * d-pad menu emulation or the menu-toggle button (chunk 3), weapon-switch
 * flick / bullet-time / laser-sight toggle (chunk 4) — see reports/06 §5 for
 * the full chunk boundary list.
 *
 * cl_input.c's VR cvar block (vr_yawmode, cl_walkdirection, cl_comfort,
 * cl_yawmult/cl_pitchmult, cl_righthanded, cl_movementspeed/cl_movespeedkey)
 * and CL_AdjustAngles are unmodified engine code this file's output feeds.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef IN_LOCOMOTION_H_
#define IN_LOCOMOTION_H_

/* One-time setup: registers the vr_locodebug console command (mirrors
 * webxr_input.c's vr_inputdebug — a live cl.viewangles/cl.movement_origin
 * dump used by the M3 loco test harness to verify the player actually
 * turned/moved, not just that input bits crossed JS->C). */
void WebXRLoco_Init(void);

/* VR session-start hook (called once from webxr_bridge.c's
 * WebXRBridge_OnSessionStart). main_web.c's flatscreen boot config
 * force-sets vr_yawmode 0 (absolute/swivel-chair, for desktop mouse-look)
 * unconditionally at startup, before any VR session exists — which would
 * otherwise permanently disable snap/stick-turn in VR too, since that
 * Cbuf_AddText runs regardless of whether the session ever goes immersive.
 * Restores the fork's own cvar-declared VR default (vr_yawmode 1 = snap,
 * cl_input.c:438) the first time a session starts, without fighting a
 * player's own later choice via the VR options menu (only acts when the
 * cvar is still at the flatscreen-forced 0). */
void WebXRLoco_OnSessionStart(void);

/* Per-XR-frame locomotion update. Call once per XR frame, AFTER
 * WebXRInput_Update() has refreshed this frame's controller state and
 * (once chunk 2 lands) after gunangles has been set for this frame — the
 * off-hand heading math and positional-delta direction both reference the
 * current gunangles[YAW] exactly as HandleInput_Default did. Safe to call
 * before chunk 2 lands too: gunangles falls back to the M2 placeholder
 * (mirrors hmdorientation), which degrades gracefully to head-relative
 * movement. Owns the leftTrackedRemoteState_old / rightTrackedRemoteState_old
 * copy-back for the hands/fields it reads (see .c file merge note if other
 * chunks also copy old<-new for the same hand). No-op outside a session
 * (controller state reads as inactive/zero, so movement/turn output is 0).
 */
void WebXRLoco_Update(void);

/* Session teardown (called from WebXRInput_Reset on session end): releases
 * keys this module may be holding down — K_SPACE (jump, right-hand A held
 * across the exit) and K_SHIFT (off-hand run trigger) — and clears the
 * private edge-detection state. */
void WebXRLoco_SessionEnd(void);

#endif
