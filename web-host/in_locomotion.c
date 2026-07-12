/*
 * in_locomotion.c — M3 chunk 1: VR locomotion & turning.
 *
 * See in_locomotion.h for scope. Ports the locomotion portions of
 * QuakeQuestSrc/QuakeQuest_OpenXR.c's HandleInput_Default() (the Android VR
 * host's per-frame input function, called once per frame from
 * VR_HandleControllerInput() after TBXR_UpdateControllers() —
 * QuakeQuest_OpenXR.c:632-1023/1025-1029) onto the WebXR controller-input
 * foundation (webxr_input.h). Consumes leftTrackedRemoteState_new/_old,
 * rightTrackedRemoteState_new/_old, leftRemoteTracking_new/rightRemoteTracking_new
 * exactly as the original consumed the OpenXR-backed globals of the same
 * names/shapes (PORT_NOTES.md "M3 foundation").
 *
 * IMPORTANT: darkplaces/vid_android.c (QC_MoveEvent/QC_MotionEvent/QC_Analog/
 * IN_Move, the comfortInc snap-turn wraparound) is compiled into this build
 * BYTE-IDENTICAL to the Android fork (confirmed via diff against
 * QuakeQuest/Projects/Android/jni/darkplaces/vid_android.c) — despite
 * reports/06 classifying it (b)/rewrite, it turned out to have zero Android
 * dependencies (PORT_NOTES.md "Architecture": "vid_android.c is compiled
 * as-is"). Likewise cl_input.c's VR cvar block + CL_AdjustAngles (comfortInc
 * consumption, vr_yawmode dispatch) is byte-identical, unmodified. So this
 * file's job is narrower than reports/06 §5 Chunk 1 implies: it does NOT
 * need to reimplement QC_MoveEvent/QC_MotionEvent/QC_Analog/IN_Move — it
 * only needs to CALL them with the right values, the way HandleInput_Default
 * did. webxr_bridge.c's OnXRFrame already calls
 * QC_MoveEvent(hmdorientation[YAW], hmdorientation[PITCH], hmdorientation[ROLL])
 * every XR frame unconditionally (M2), which is the exact call
 * AppThreadFunction made every Android frame (QuakeQuest_OpenXR.c:282) —
 * NOT part of HandleInput_Default, so this file does not touch it. What was
 * missing (and is this file's actual deliverable) is the QC_MotionEvent call
 * (turn-stick -> comfortInc / stick-turn) and the QC_Analog call (movement
 * stick + positional delta -> cl.cmd sidemove/forwardmove via
 * cl_input.c:411-421's analog special-case in CL_KeyState), both of which
 * only existed inside HandleInput_Default on Android and have no other home
 * in this port.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <emscripten.h>

#include "quakedef.h" /* vec2_t/vec3_t, matrix4x4_t, Matrix4x4_*, cl, cvar_t, YAW/PITCH/ROLL, Con_Printf, dpsnprintf, Cmd_AddCommand */
#include "keys.h"     /* K_SHIFT, K_MOUSE1, K_SPACE */

#include "webxr_input.h"
#include "webxr_bridge.h" /* WebXRBridge_QuatToYawPitchRoll — the same ported
                            * TBXR_Common.c:911-938 coordinate remap chunk 2
                            * uses for gunangles; reused here (not duplicated)
                            * for the off-hand controller-relative heading. */

#include "in_locomotion.h"

/* ---- engine externs (ad hoc extern is the established pattern in this
 * codebase — see view.c:125-126, menu.c:53-55 doing the same for these two
 * cvars, which client.h does not declare) ---- */
extern cvar_t cl_righthanded;
extern cvar_t cl_walkdirection;

/* client.h already declares: cl_movementspeed, cl_movespeedkey, vr_yawmode,
 * cl_comfort (extern cvar_t). cl (client_state_t) via quakedef.h->client.h. */

extern float gunangles[3];       /* view.c — this frame's weapon-aim angles (chunk 2) */
extern float hmdorientation[3];  /* main_web.c/webxr_bridge.c — [pitch,yaw,roll] degrees */

qboolean VR_UseScreenLayer(void); /* main_web.c — the fork's 2D-UI predicate
                                   * (bigScreen/demo/console); gates the jump
                                   * binding to in-game frames like the fork's
                                   * bigScreen == 0 else-branch did */

/* vid_android.c (compiled unmodified — see file header) */
void QC_Analog(int enable, float x, float y);
void QC_MotionEvent(float delta, float dx, float dy);

/* =====================================================================
 * Pure math, ported verbatim from QuakeQuest_OpenXR.c (reports/06 §5
 * Chunk 1: "copy verbatim").
 * ===================================================================== */

#define NLF_DEADZONE 0.1f
#define NLF_POWER    2.2f

/* QuakeQuest_OpenXR.c:574-593 */
static float LocoNonLinearFilter(float in)
{
    float val = 0.0f;
    if (in > NLF_DEADZONE)
    {
        val = in > 1.0f ? 1.0f : in;
        val -= NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = powf(val, NLF_POWER);
    }
    else if (in < -NLF_DEADZONE)
    {
        val = in < -1.0f ? -1.0f : in;
        val += NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = -powf(fabsf(val), NLF_POWER);
    }
    return val;
}

/* QuakeQuest_OpenXR.c:600-603 */
static float LocoLength(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

/* QuakeQuest_OpenXR.c:617-630 — rotate (v1,v2) by `rotation` degrees about
 * the origin (used both for heading-relative movement and for un-rotating
 * the HMD positional delta into gunangles-relative space). */
static void LocoRotateAboutOrigin(float v1, float v2, float rotation, vec2_t out)
{
    vec3_t temp;
    temp[0] = v1;
    temp[1] = v2;
    temp[2] = 0.0f;

    vec3_t v;
    matrix4x4_t matrix;
    Matrix4x4_CreateFromQuakeEntity(&matrix, 0.0f, 0.0f, 0.0f, 0.0f, rotation, 0.0f, 1.0f);
    Matrix4x4_Transform(&matrix, temp, v);

    out[0] = v[0];
    out[1] = v[1];
}

/* QuakeQuest_OpenXR.c:609-615 handleTrackedControllerButton — edge-detected
 * button -> QC_KeyEvent. Local copy (trivial, 5 lines); chunk 2/3/4 may keep
 * their own rather than share a header for this — not worth a shared-file
 * edit over. */
void QC_KeyEvent(int state, int key, int character); /* vid_android.c -> Key_Event */

static void LocoHandleButtonEdge(const WebXRRemoteState *st, const WebXRRemoteState *old,
                                 uint32_t button, int key)
{
    if ((st->Buttons & button) != (old->Buttons & button))
        QC_KeyEvent((st->Buttons & button) != 0 ? 1 : 0, key, 0);
}

/* =====================================================================
 * Per-frame locomotion update.
 * ===================================================================== */

static double s_locoLastMs = -1.0;

/* PRIVATE previous right-hand state for the jump-button edge (same pattern
 * as in_weapon.c/in_menu.c: the shared rightTrackedRemoteState_old is not
 * used for edges by this module to avoid racing the other chunks' copies) */
static WebXRRemoteState s_locoPrevRight;

static void WebXRLoco_DebugTick(void); /* below (needs EM_JS) */

void WebXRLoco_Update(void)
{
    double now = emscripten_get_now();
    float frameMs = (s_locoLastMs >= 0.0) ? (float)(now - s_locoLastMs) : 16.0f;
    if (frameMs <= 0.0f) frameMs = 16.0f;
    if (frameMs > 1000.0f) frameMs = 1000.0f;
    s_locoLastMs = now;

    /* QuakeQuest_OpenXR.c:595-598 GetSysTicrate() derives the refresh rate
     * from TBXR_GetRefresh() (an OpenXR call with no WebXR equivalent
     * exposed by the bridge yet); measuring it from the frame delta is the
     * portable substitute and converges to the same value. */
    float refreshHz = 1000.0f / frameMs;
    if (refreshHz < 30.0f) refreshHz = 30.0f;
    if (refreshHz > 144.0f) refreshHz = 144.0f;

    float remote_movementSideways = 0.0f;
    float remote_movementForward = 0.0f;
    float positional_movementSideways = 0.0f;
    float positional_movementForward = 0.0f;

    /* QuakeQuest_OpenXR.c:641 — the amount of yaw the "logical" facing
     * (cl.viewangles[YAW], driven by snap/stick-turn) has diverged from the
     * raw physical HMD yaw. Used to rotate movement/weapon vectors so they
     * stay relative to logical facing regardless of which way the player's
     * head is physically turned. Independent of gunangles (chunk 2). */
    float yawOffset = cl.viewangles[YAW] - hmdorientation[YAW];

    /* QuakeQuest_OpenXR.c:643-648 — dominant/off-hand selection.
     * NOTE: only the off-hand pointer (used for the controller-relative
     * heading, cl_walkdirection==0) is handedness-selected here, exactly as
     * the original did; the movement thumbstick itself is read from the
     * LITERAL left controller and the turn analog from the LITERAL right
     * controller regardless of cl_righthanded (see LEFT-HAND/RIGHT-HAND
     * blocks below) — a quirk of the original code preserved faithfully
     * (see PORT_NOTES/report for this chunk: for a left-handed player,
     * cl_walkdirection==0 "walk toward off-hand controller" then reads the
     * RIGHT controller's orientation but the LEFT controller's stick axes).
     */
    const WebXRTrackedController *offHandRemoteTracking =
        cl_righthanded.integer ? &leftRemoteTracking_new : &rightRemoteTracking_new;

    /* QuakeQuest_OpenXR.c:799-808 — off-hand stuff: heading candidates for
     * cl_walkdirection. */
    float controllerYawHeading, hmdYawHeading;
    {
        float q[4] = {
            offHandRemoteTracking->Pose.orientation.x,
            offHandRemoteTracking->Pose.orientation.y,
            offHandRemoteTracking->Pose.orientation.z,
            offHandRemoteTracking->Pose.orientation.w,
        };
        float controllerAngles[3];
        WebXRBridge_QuatToYawPitchRoll(q, NULL, controllerAngles);

        controllerYawHeading = controllerAngles[YAW] - gunangles[YAW] + yawOffset;
        hmdYawHeading = hmdorientation[YAW] - gunangles[YAW] + yawOffset;
    }

    /* QuakeQuest_OpenXR.c:810-835 — "Right-hand specific stuff": HMD
     * positional-delta walking + the turn-stick feed into QC_MotionEvent.
     * Both are read from the LITERAL right controller / HMD in the
     * original, not the dominant/off-hand pointers. */
    {
        float posDelta[3];
        WebXRInput_GetHMDPositionDelta(posDelta);

        /* off-hand trigger held -> use cl_movespeedkey (run) multiplier,
         * exactly as the fork gated the positional-walk speed on the
         * off-hand run trigger too (QuakeQuest_OpenXR.c:820). */
        const WebXRRemoteState *offHandState =
            cl_righthanded.integer ? &leftTrackedRemoteState_new : &rightTrackedRemoteState_new;
        float multiplier = (2300.0f * (refreshHz / 72.0f)) /
            (cl_movementspeed.value * ((offHandState->Buttons & xrButton_Trigger) ? cl_movespeedkey.value : 1.0f));

        vec2_t v;
        LocoRotateAboutOrigin(-posDelta[0] * multiplier, posDelta[2] * multiplier,
                              yawOffset - gunangles[YAW], v);
        positional_movementSideways = v[0];
        positional_movementForward = v[1];

        /* QuakeQuest_OpenXR.c:829-835 — turn stick (always the physical
         * right controller): feeds vid_android.c's QC_MotionEvent, which
         * drives cl.comfortInc (vr_yawmode 1, snap) or in_mouse_x
         * (vr_yawmode 2, stick) — both unmodified engine code. */
        QC_MotionEvent(frameMs, rightTrackedRemoteState_new.Joystick.x,
                       rightTrackedRemoteState_new.Joystick.y);

        /* bigScreen d-pad menu-navigation emulation (QuakeQuest_OpenXR.c:
         * 837-862) is chunk 3's (HUD & Menus) responsibility — not ported
         * here to avoid duplicate/conflicting Key_Event dispatch. */

        /* WEBXR-PORT M3 gap fix (headset QA bug 1): Jump — right-hand A ->
         * K_SPACE, the fork's in-game else-branch of the bigScreen split
         * (QuakeQuest_OpenXR.c:863-866). Was never ported by any chunk:
         * chunk 3 owns A only WHILE the menu is up (A = K_ENTER, fork
         * :856-858) and no other chunk touched A in-game. Gated by the same
         * 2D-UI predicate the fork used (bigScreen -> VR_UseScreenLayer,
         * which also covers demo/console frames). The fork's in-game B
         * binding is explicitly "//Unused" (:869-874) — nothing to port.
         * K_SPACE is bound to +jump by main_web.c's init binds. */
        if (!VR_UseScreenLayer())
            LocoHandleButtonEdge(&rightTrackedRemoteState_new, &s_locoPrevRight,
                                 xrButton_A, K_SPACE);
    }

    /* QuakeQuest_OpenXR.c:906-963 — "Left-hand specific stuff": movement
     * thumbstick (deadzone/curve filter -> heading rotation) + the off-hand
     * run/fire trigger binding. Both hardcoded to the LITERAL left
     * controller in the original (see note above). */
    {
        const WebXRRemoteState *left = &leftTrackedRemoteState_new;

        /* QuakeQuest_OpenXR.c:936-951 — nonlinear deadzone/curve filter so
         * small deflections are controllable and centering jitter doesn't
         * cause movement noise, then rotate into world space by whichever
         * heading cl_walkdirection selects. */
        float dist = LocoLength(left->Joystick.x, left->Joystick.y);
        float nlf = LocoNonLinearFilter(dist);
        float distClamped = (dist > 1.0f) ? dist : 1.0f;
        float x = (distClamped > 0.0f) ? nlf * (left->Joystick.x / distClamped) : 0.0f;
        float y = (distClamped > 0.0f) ? nlf * (left->Joystick.y / distClamped) : 0.0f;

        vec2_t v;
        LocoRotateAboutOrigin(x, y,
                              cl_walkdirection.integer == 1 ? hmdYawHeading : controllerYawHeading,
                              v);
        remote_movementSideways = v[0];
        remote_movementForward = v[1];

        /* menu-toggle (Enter button) and the bigScreen d-pad branch
         * (QuakeQuest_OpenXR.c:912-933) are chunk 3's — not ported here. */

        /* QuakeQuest_OpenXR.c:953-963 — off-hand run/fire trigger binding,
         * hardcoded to the literal left controller (mirrors the dominant
         * fire/run binding chunk 2 owns on the literal right controller,
         * QuakeQuest_OpenXR.c:889-899). */
        if (cl_righthanded.integer)
            LocoHandleButtonEdge(left, &leftTrackedRemoteState_old, xrButton_Trigger, K_SHIFT);   /* Run */
        else
            LocoHandleButtonEdge(left, &leftTrackedRemoteState_old, xrButton_Trigger, K_MOUSE1);  /* Fire */

        /* QuakeQuest_OpenXR.c:1004 — save state for next frame's edge
         * detection. MERGE NOTE: if another chunk also edge-detects left-
         * hand buttons this frame (e.g. chunk 3's menu toggle / bigScreen
         * d-pad), make sure only ONE old<-new copy per hand happens per
         * frame, after all readers — see in_locomotion.h / report. */
        leftTrackedRemoteState_old = leftTrackedRemoteState_new;
    }

    /* QuakeQuest_OpenXR.c:1007-1008 — combine thumbstick + positional-delta
     * contributions into the single QC_Analog call CL_KeyState's analog
     * special-case (cl_input.c:411-421) turns into cl.cmd side/forwardmove. */
    QC_Analog(true, remote_movementSideways + positional_movementSideways,
             remote_movementForward + positional_movementForward);

    /* rightTrackedRemoteState_old is only read by chunk 2 (fire trigger,
     * literal right hand) and chunk 4 (weapon-switch flick) in the original
     * split — this file only READS rightTrackedRemoteState_new (turn stick,
     * analog, no edge detection needed), so it does not own that copy. The
     * jump edge above uses this module's PRIVATE right-hand copy instead;
     * updated unconditionally (even on 2D-UI frames) so no stale edge fires
     * when the menu closes with A held. */
    s_locoPrevRight = rightTrackedRemoteState_new;

    WebXRLoco_DebugTick();
}

/* Session teardown: release anything this module may be holding down so it
 * can't leak into flatscreen (K_SPACE = jump edge above; K_SHIFT = the
 * off-hand run trigger, held-across-exit case). Called from
 * WebXRInput_Reset alongside IN_Weapon_SessionEnd. */
void WebXRLoco_SessionEnd(void)
{
    QC_KeyEvent(0, K_SPACE, 0);
    QC_KeyEvent(0, K_SHIFT, 0);
    memset(&s_locoPrevRight, 0, sizeof(s_locoPrevRight));
}

/* =====================================================================
 * vr_locodebug — live cl.viewangles/comfortInc/cl.movement_origin dump
 * (mirrors webxr_input.c's vr_inputdebug pattern: DOM overlay for headless
 * test assertions + 1Hz Con_Printf notify lines visible in-headset).
 * ===================================================================== */

static int s_locoDebug = 0;
static double s_locoDebugOverlayMs = 0.0;
static double s_locoDebugConsoleMs = 0.0;

EM_JS(void, webxr_js_locodebug_overlay, (const char *txt), {
    var el = document.getElementById('qq-locodebug');
    if (!el) {
        el = document.createElement('pre');
        el.id = 'qq-locodebug';
        el.style.cssText = 'position:fixed;top:0;left:0;z-index:1000;' +
            'background:rgba(0,0,0,.85);color:#9cf;font:11px/1.4 monospace;' +
            'padding:6px 8px;margin:0;pointer-events:none;white-space:pre;' +
            'text-align:left;max-width:60ch;';
        document.body.appendChild(el);
    }
    el.textContent = UTF8ToString(txt);
});

EM_JS(int, webxr_js_locodebug_param, (void), {
    try {
        return new URLSearchParams(location.search).get('locodebug') ? 1 : 0;
    } catch (e) { return 0; }
});

static void LocoFormatState(char *buf, int size)
{
    dpsnprintf(buf, size,
        "-- vr loco --\n"
        "viewangles(p=%6.1f y=%6.1f r=%6.1f) comfortInc=%d (%.1f deg/step)\n"
        "origin(%7.1f %7.1f %7.1f) vr_yawmode=%d cl_walkdirection=%d\n",
        cl.viewangles[PITCH], cl.viewangles[YAW], cl.viewangles[ROLL],
        cl.comfortInc, cl_comfort.value,
        cl.movement_origin[0], cl.movement_origin[1], cl.movement_origin[2],
        vr_yawmode.integer, cl_walkdirection.integer);
}

static void WebXRLoco_DebugTick(void)
{
    if (!s_locoDebug)
        return;

    double now = emscripten_get_now();
    char buf[256];

    if (now - s_locoDebugOverlayMs > 200.0)
    {
        s_locoDebugOverlayMs = now;
        LocoFormatState(buf, sizeof(buf));
        webxr_js_locodebug_overlay(buf);
    }
    if (now - s_locoDebugConsoleMs > 1000.0)
    {
        s_locoDebugConsoleMs = now;
        LocoFormatState(buf, sizeof(buf));
        Con_Printf("%s", buf);
    }
}

static void WebXRLoco_DebugCmd_f(void)
{
    int enable = (Cmd_Argc() > 1) ? atoi(Cmd_Argv(1)) : !s_locoDebug;
    s_locoDebug = enable;
    if (!enable)
        webxr_js_locodebug_overlay("");
    Con_Printf("vr_locodebug %s\n", enable ? "ON (overlay + 1Hz notify)" : "off");
}

void WebXRLoco_Init(void)
{
    Cmd_AddCommand("vr_locodebug", WebXRLoco_DebugCmd_f,
                   "toggle live VR locomotion state dump (viewangles/comfortInc/origin)");
    if (webxr_js_locodebug_param())
        s_locoDebug = 1;
    printf("[webxr] locomotion (M3 chunk 1) initialised\n");
}

void WebXRLoco_OnSessionStart(void)
{
    /* see in_locomotion.h for why this only fires when still at the
     * flatscreen-forced value */
    if (vr_yawmode.integer == 0)
    {
        Cvar_SetValueQuick(&vr_yawmode, 1);
        printf("[webxr] locomotion: restored vr_yawmode 1 (snap-turn) for VR session\n");
    }
}
