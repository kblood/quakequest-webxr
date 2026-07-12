/*
 * in_comfort.h — VR comfort & calibration (Milestone 3, chunk 4).
 *
 * Ports reports/06-vr-gameplay-map.md §4 (Comfort & misc) + the recenter/
 * height-calibration item from §4's table, decomposed as chunk 4 in §5.
 * Everything here is self-contained: it reads the M3 input foundation's
 * per-hand state (webxr_input.h) directly.
 *
 * Covers:
 *  - Recenter/height calibration: lib/webxr PATCH #14 (webxr_recenter(),
 *    JS-side getOffsetReferenceSpace swap) does the actual work; this module
 *    supplies WebXRComfort_Recenter() (re-latches playerHeight on success)
 *    and the `vr_recenter` console command. The fork's TBXR_Recenter had no
 *    in-game control (system-event-only) — see reports/08d-comfort.md.
 *    M3 INTEGRATION CHANGE (reports/09-m3-integration.md): this chunk's
 *    original in-game trigger was the off-hand thumbstick CLICK, which
 *    collided with chunk 3's menu toggle (in_menu.c chose the same input —
 *    both chunks independently picked "the one input the fork left free").
 *    Resolution: in_menu.c owns the whole off-hand-click gesture now —
 *    short press = menu toggle, LONG press (>= 600 ms, in-game only) calls
 *    WebXRComfort_Recenter(). This module no longer reads the click at all.
 *  - Quicksave/quickload (physical left X/Y): REMOVED in headset-QA
 *    round 2 (was a deliberate dead-code fix of QuakeQuest_OpenXR.c:965-983;
 *    real-device QA vetoed save/load on bare face buttons — accidental
 *    presses destroy progress). No controller button saves or loads;
 *    menu-based save/load remains. See the block comment in in_comfort.c.
 *  - Bullet-time/slow-mo speed computation — port of
 *    QuakeQuest_OpenXR.c:1011-1021 (engine-side consumption in cl_input.c is
 *    untouched/already-compiled).
 *
 * NOT here (left to whichever chunk owns them, see report):
 *  - Laser-sight cycle toggle (dominant thumbstick click,
 *    QuakeQuest_OpenXR.c:790-795) and weapon-switch joystick-flick
 *    (physical right stick Y, QuakeQuest_OpenXR.c:876-886): reports/06 §5
 *    listed both under chunk 2 as well as chunk 4. This module HAD working
 *    ports of both, but chunk 2 (branch m3-weapon, reports/08b-weapon.md)
 *    landed first and explicitly claims both, warning chunk 4 off
 *    duplicating the weapon-switch flick specifically ("dupe symptom:
 *    double weapon switches per flick") — removed here before finalizing;
 *    see reports/08d-comfort.md "Discovered cross-chunk duplication".
 *  - `bigScreen`-gating for the above: moot now that they're not
 *    implemented here; chunk 2's report notes the same TODO for its own
 *    copy once chunk 3's menu-quad state lands.
 *  - `cl_controllerdeadzone` (dead cvar, cl_input.c:445): wiring it requires
 *    editing chunk 1's nonLinearFilter(), which doesn't exist on this
 *    branch. Recommendation only (see reports/08d-comfort.md), not wired.
 *  - vr_worldscale / positional scale: engine-side, unmodified, verified
 *    end-to-end (report §4: "No changes needed"); no new code here.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */
#ifndef IN_COMFORT_H_
#define IN_COMFORT_H_

/* One-time setup after Host_Main(): registers `vr_recenter` (console command
 * fallback, also the harness-testable surface for the recenter trigger). */
void WebXRComfort_Init(void);

/* Recenter (yaw + XZ position via lib/webxr PATCH #14) and re-latch
 * playerHeight. Triggers: the `vr_recenter` console command (here) and the
 * off-hand thumbstick LONG-press (in_menu.c, which owns the off-hand click
 * gesture — see the M3 integration note above). Safe no-op outside a
 * session (webxr_recenter() returns 0 without a reference space). */
void WebXRComfort_Recenter(void);

/* Called once per XR frame from webxr_bridge.c's OnXRFrame, after
 * WebXRInput_Update (needs this frame's fresh button/axis state) and before
 * QC_BeginFrame. No-op (all-zero reads) outside an XR session. */
void WebXRComfort_Update(void);

#endif
