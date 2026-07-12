/*
 * in_weapon.h — M3 chunk 2: VR weapon aim, firing & haptics (WEBXR-PORT M3-weapon).
 *
 * Ports the dominant-hand block of QuakeQuestSrc/QuakeQuest_OpenXR.c's
 * HandleInput_Default() (lines 746-796: weaponOffset/weaponVelocity/gunangles
 * production + two-handed stabilization + laser-sight cycle), the fire-trigger
 * bindings (889-899 dominant side only), the weapon-switch joystick flick
 * (876-886) and VR_HapticEvent's per-weapon rumble table (369-467), against
 * the M3 input foundation (webxr_input.h) instead of OpenXR.
 *
 * The consuming engine chain is untouched and already compiled in:
 *   view.c:860-998 (per-weapon matrix/offset table, vr_worldscale scaling)
 *   cl_input.c:1845 (cl.cmd.viewangles = gunangles)
 *   sv_phys.c:3005-3043 (fire-origin swap to gunorg)
 * This module's entire job is producing correct weaponOffset / weaponVelocity /
 * gunangles / weapon_stabilised values each XR frame.
 *
 * Edge detection uses PRIVATE previous-state copies (NOT the shared
 * leftTrackedRemoteState_old globals) so the four parallel M3 chunks cannot
 * corrupt each other's edges regardless of dispatch order.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef IN_WEAPON_H_
#define IN_WEAPON_H_

#include <stdbool.h>

/* One-time setup after Host_Main(): queues the fork's weapon-cycle key binds
 * (assets/config.cfg: bind / "impulse 10", bind # "impulse 12").
 * Called from WebXRInput_Init(). */
void IN_Weapon_Init(void);

/* Per-XR-frame weapon input. Called from WebXRInput_Update() after the
 * TBXR-compatible *_new state is built. nowMs = emscripten_get_now(). */
void IN_Weapon_Update(double nowMs);

/* Session ended: release held weapon keys, clear state, restore the
 * flatscreen 3DoF tracking mode. Called from WebXRInput_Reset(). */
void IN_Weapon_SessionEnd(void);

/* True when the controller drove gunangles this frame — webxr_bridge.c's
 * head-aim (M2 behavior) is only applied as a fallback when this is false. */
bool IN_Weapon_AimActive(void);

/* Ported fork haptics entry point (QuakeQuest_OpenXR.c:369-467): per-weapon
 * rumble while the fire trigger is held, channel = both hands when
 * weapon-stabilised. Signature kept identical to the fork's so any future
 * call sites port verbatim; the per-frame null-params call happens inside
 * IN_Weapon_Update (the fork called it from its main loop). */
void VR_HapticEvent(const char *event, int position, int flags,
                    int intensity, float angle, float yHeight);

/* Fork globals this module now owns (QuakeQuest_OpenXR.c:50-51).
 * weaponVelocity: dominant-hand linear velocity, m/s, raw XR axes —
 * chunk 4's bullet-time computation reads this.
 * weapon_stabilised: two-handed grip active (chunk 4 haptic channel select
 * already handled here). Defined as bool (fork used qboolean; nothing
 * engine-side links against it). */
extern float weaponVelocity[3];
extern bool weapon_stabilised;

#endif
