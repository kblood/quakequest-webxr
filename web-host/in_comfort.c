/*
 * in_comfort.c — VR comfort & calibration (Milestone 3, chunk 4).
 *
 * See in_comfort.h for the contract/scope. Reads the M3 input foundation's
 * TBXR-compatible per-hand state (webxr_input.h) directly; does not depend
 * on any other M3 chunk's code being present.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <stdio.h>
#include <math.h>

#include <emscripten.h>

#include "quakedef.h"

#include "webxr_input.h"
#include "in_comfort.h"
#include "in_locomotion.h"   /* WebXRLoco_GetEyeOffset — duck-aware height re-latch */
#include "lib/webxr/webxr.h" /* webxr_recenter — PATCH #14 */

/* ---- engine entry points / externs ---- */
extern cvar_t cl_righthanded;      /* cl_input.c:446 — which physical hand aims/leads */
extern cvar_t r_lasersight;        /* gl_rmain.c:129 */
extern cvar_t bullettime;          /* sv_main.c:51 */
extern cvar_t slowmo;              /* sv_main.c:50 */
extern cvar_t vr_worldscale;       /* gl_rmain.c:52 — Quake-units-per-real-meter; height/positional scale */

extern float hmdPosition[3];       /* webxr_bridge.c (raw XR head position, meters) */
extern float playerHeight;         /* main_web.c — standing-height reference (view.c:929) */

/* =====================================================================
 * Recenter / height calibration
 *
 * The fork's TBXR_Recenter (TBXR_Common.c:1818-1860) rebuilds the OpenXR
 * reference space — STAGE if the runtime supports it, else LOCAL with a
 * hardcoded -1.675m floor offset — and is invoked reactively from the
 * runtime's own recenter gesture (ovrApp_HandleXrEvents seeing
 * XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING). WebXR's 'local-floor'
 * session feature (requested at session start, webxr_bridge.c
 * WebXRBridge_RequestSession) already reproduces BOTH halves of that for
 * free: the browser supplies a real tracked floor when available and a
 * conventional assumed-floor fallback (~1.6m, spec-mandated) when it isn't
 * — the exact shape of the fork's STAGE-vs-fake-stage fallback, just
 * implemented by the UA instead of by us. And because it's a *reference
 * space*, a platform-level system recenter (long-press the Quest's Oculus
 * button) updates it transparently too, same as the fork's event-driven
 * path — no C-side code needed for that case either.
 *
 * What the fork never had is an in-game trigger for a recenter (grepped
 * the whole fork tree: zero hits on any button calling TBXR_Recenter).
 * That's a real gap for a browser tab, where a system-level recenter
 * gesture is far less discoverable than it was in the native Android app,
 * so this chunk adds one. M3 INTEGRATION CHANGE: the original binding here
 * (off-hand thumbstick CLICK) collided with chunk 3's menu toggle — both
 * chunks independently picked the same "only free input" (see
 * reports/09-m3-integration.md). The click gesture is now owned entirely
 * by in_menu.c: short press = menu, LONG press (>= 600 ms, in-game only)
 * calls WebXRComfort_Recenter() below. The `vr_recenter` console command
 * remains as a keyboard/harness-reachable fallback.
 * See lib/webxr/PATCHES.md #14 for the getOffsetReferenceSpace
 * mechanics (why it's done JS-side against the shared reference space
 * rather than as a C-side position offset: a C-side-only offset on
 * hmdPosition would desync from controller positions, which are tracked
 * independently in webxr_input.c, and break the controller-minus-head
 * weapon math — the JS reference-space swap keeps every consumer,
 * including chunk 2's weapon aim, in the same frame automatically).
 * ===================================================================== */
void WebXRComfort_Recenter(void)
{
    if (webxr_recenter())
    {
        /* Re-latch the height baseline too: playerHeight is normally only
         * captured once, on the flatscreen<->VR transition (webxr_bridge.c
         * VR_SetHMDPosition). If that first sample was off (sitting play,
         * a child user, standing crooked when entering VR), recenter is the
         * natural "fix my height" moment as well — "stand up straight and
         * press recenter". The fork never did this (its recenter never
         * touched playerHeight either), but it costs nothing and matches
         * user expectation for what a recenter button does.
         * hmdPosition[1] already has the duck eye offset subtracted
         * (headset-QA round 2); add it back so recentering while the duck
         * button happens to be held can't lower the standing baseline. */
        playerHeight = hmdPosition[1] + WebXRLoco_GetEyeOffset();
        SCR_CenterPrint("Recentered");
        Con_Printf("[comfort] recentered (yaw + position; height re-latched to %.1f)\n", playerHeight);
    }
}

static void WebXRComfort_Recenter_f(void)
{
    WebXRComfort_Recenter();
}

/* =====================================================================
 * Quicksave / quickload on X/Y — REMOVED (headset-QA round 2, item 3).
 *
 * The M3-chunk-4 port deliberately "fixed" the fork's dead
 * canUseQuickSave code (QuakeQuest_OpenXR.c:965-983, guard never set true
 * — reports/06 §1.2) by making left X = `save quick` and left Y =
 * `load quick` live. Real-headset QA vetoed it: X/Y sit exactly where the
 * thumb rests, and an ACCIDENTAL quickload silently discards progress (an
 * accidental quicksave overwrites the good slot) — state-destroying
 * actions must not live on bare face buttons. Both bindings are gone; no
 * controller button may save or load. Save/load remain reachable through
 * the in-VR menu (d-pad nav, in_menu.c) and the keyboard F6/F9 binds
 * (shareware default.cfg) on flatscreen.
 * Left X is reused as the PRIMARY menu toggle (in_menu.c, QA round 2
 * item 2); left Y is deliberately UNBOUND — if it ever gets a role, it
 * must be a harmless one (never destructive/state-changing).
 * ===================================================================== */

/* =====================================================================
 * Laser-sight cycle toggle and weapon-switch stick-flick — INTENTIONALLY
 * NOT HERE. reports/06 §5 listed both under chunk 2 as well as chunk 4;
 * chunk 2 (branch m3-weapon, reports/08b-weapon.md) landed first and
 * explicitly claims both:
 *   - "dominant thumbstick click -> r_lasersight = (v+1)%3" (in_weapon.c,
 *     part of the same dominant-hand block that computes weaponOffset/
 *     gunangles — 08b-weapon.md's data-flow diagram and button map).
 *   - "RIGHT stick y flick (>+-0.7, not screen-layer) -> keys '/'/'#' ->
 *     binds impulse 10/12 (injected by IN_Weapon_Init)" (08b-weapon.md
 *     line 38-39), with an explicit note to chunk 4: "it is implemented
 *     HERE; chunk 4 should not duplicate it (dupe symptom: double weapon
 *     switches per flick)" (08b-weapon.md line 113-115). Chunk 3's report
 *     (08c-hud-menus.md, "Crosshair / laser sight") likewise defers the
 *     laser-sight toggle to chunk 2.
 * This module had working implementations of both (right up through the
 * IWER-verified test run) until this cross-check against the sibling
 * reports; removed before finalizing rather than leave a known merge
 * conflict for the orchestrator. See reports/08d-comfort.md "Discovered
 * cross-chunk duplication" for the full account, including a real fix
 * found while it was still here: this port's preloaded shareware pak0.pak
 * only binds "/" by default (`bind #` -> "#" is not bound"), so a
 * key-event-based weapon-switch silently only works in one direction
 * unless something binds/sends the impulse directly — chunk 2's
 * IN_Weapon_Init() bind-injection and this module's now-removed
 * Cbuf_InsertText("impulse NN\n") approach both independently found and
 * fixed the same gap; flagging in case IN_Weapon_Init()'s fix needs
 * double-checking at merge time.
 * ===================================================================== */

/* =====================================================================
 * Bullet-time / slow-mo — QuakeQuest_OpenXR.c:1011-1021. Engine-side
 * consumption (cl_input.c:1550-1551, sv_main.c/host.c slowmo scaling) is
 * already compiled and untouched; this only reproduces the speed
 * computation that feeds the `slowmo` cvar.
 *
 * Note this reads leftTrackedRemoteState_new.Joystick unconditionally (the
 * fork hardcodes LEFT here too, same pattern as the weapon-switch block
 * above hardcoding RIGHT) — under the default cl_righthanded 1, left is
 * the off-hand/movement stick, so "how hard is the player pushing the
 * movement stick" is the intended read; ported as-is rather than made
 * handedness-relative, matching the fork exactly.
 *
 * weaponVelocity: the fork's HandleInput_Default wrote a host-side global
 * `weaponVelocity[3]` from the dominant controller's OpenXR velocity and
 * read it back here (QuakeQuest_OpenXR.c:50,761-763,1015) — but that global
 * is host-only (never referenced anywhere in darkplaces/, confirmed via
 * grep), so there is nothing engine-side to link against and nothing
 * chunk 2 needs to expose for this to work: this chunk reads the same
 * underlying data directly from the M3 foundation's per-hand
 * Velocity.linearVelocity (webxr_input.h WebXRTrackedController), which is
 * exactly what the fork's weaponVelocity was populated from in the first
 * place. Avoids a hard dependency on chunk 2 landing first.
 * ===================================================================== */
static void WebXRComfort_BulletTime(void)
{
    if (!bullettime.integer)
        return;

    float sx = leftTrackedRemoteState_new.Joystick.x;
    float sy = leftTrackedRemoteState_new.Joystick.y;
    float speed = powf(sqrtf(sx * sx + sy * sy), 1.1f);

    float posDelta[3];
    WebXRInput_GetHMDPositionDelta(posDelta);
    float movement = sqrtf(powf(posDelta[0] * 80.0f, 2) +
                            powf(posDelta[1] * 80.0f, 2) +
                            powf(posDelta[2] * 80.0f, 2));

    const WebXRTrackedController *domTC =
        cl_righthanded.integer ? &rightRemoteTracking_new : &leftRemoteTracking_new;
    float wvx = domTC->Velocity.linearVelocity.x;
    float wvy = domTC->Velocity.linearVelocity.y;
    float wvz = domTC->Velocity.linearVelocity.z;
    float weaponMovement = sqrtf(wvx * wvx + wvy * wvy + wvz * wvz);

    float maximum = max(max(speed, movement), weaponMovement);
    Cvar_SetValueQuick(&slowmo, bound(0.12f, maximum, 1.0f));
}

/* =====================================================================
 * Per-frame entry point
 * ===================================================================== */
void WebXRComfort_Update(void)
{
    WebXRComfort_BulletTime();

    /* NOTE: the off-hand thumbstick-click recenter trigger that used to
     * live here moved to in_menu.c as a LONG-press (binding collision with
     * chunk 3's menu toggle — see reports/09-m3-integration.md and the
     * header comment above WebXRComfort_Recenter). The X/Y quicksave/
     * quickload block that used to live here is gone for good — see the
     * REMOVED comment above. */
}

void WebXRComfort_Init(void)
{
    Cmd_AddCommand("vr_recenter", WebXRComfort_Recenter_f,
                   "recenter view (yaw + position) and re-latch standing height; "
                   "also bound to LONG-pressing the off-hand thumbstick in VR "
                   "(short press = menu)");
    printf("[webxr] comfort/calibration module initialised\n");
}

/* =====================================================================
 * Debug probes — test/m3-comfort-test.mjs harness surface (same convention
 * as webxr_input.c's vr_inputdebug: a small, kept-in-tree hook rather than
 * scraping console text). Callable from JS as Module._FunctionName(...).
 * ===================================================================== */
EMSCRIPTEN_KEEPALIVE
int WebXRComfort_DebugGetActiveWeapon(void)
{
    return cl.stats[STAT_ACTIVEWEAPON];
}

/* r_lasersight is written by chunk 2 (m3-weapon, in_weapon.c), not here —
 * this probe is a read-only observability hook only, kept because it's
 * useful for a future integration test once chunks merge. */
EMSCRIPTEN_KEEPALIVE
int WebXRComfort_DebugGetLaserSight(void)
{
    return r_lasersight.integer;
}

EMSCRIPTEN_KEEPALIVE
float WebXRComfort_DebugGetSlowmo(void)
{
    return slowmo.value;
}

EMSCRIPTEN_KEEPALIVE
float WebXRComfort_DebugGetWorldscale(void)
{
    return vr_worldscale.value;
}
