/*
 * webxr_input.c — WebXR controller-input foundation (Milestone 3).
 *
 * See webxr_input.h for the contract. This file:
 *  - snapshots both hands' raw controller state each XR frame
 *    (lib/webxr PATCH #12: webxr_get_controller_state),
 *  - derives the TBXR-compatible view (same globals/semantics as
 *    OpenXrInput.c's TBXR_UpdateControllers, incl. the >0.5 trigger
 *    threshold and the thumbstick y sign flip to OpenXR convention),
 *  - runs the TBXR_Vibrate-compatible haptics channels over
 *    gamepad.hapticActuators[0].pulse (lib/webxr PATCH #13),
 *  - provides the vr_inputdebug live state dump (DOM overlay + console
 *    notify lines) for binding diagnosis.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <emscripten.h>

#include "quakedef.h" /* Con_Printf, Cmd_AddCommand, dpsnprintf */

#include "webxr_input.h"

/* host globals (main_web.c / webxr_bridge.c) */
extern float hmdorientation[3];
extern float hmdPosition[3];
bool WebXRBridge_IsSessionActive(void); /* webxr_bridge.h (avoid dup include) */

/* =====================================================================
 * State
 * ===================================================================== */

WebXRControllerState webxr_controllers[WEBXR_HAND_COUNT];

WebXRRemoteState leftTrackedRemoteState_new,  leftTrackedRemoteState_old;
WebXRRemoteState rightTrackedRemoteState_new, rightTrackedRemoteState_old;
WebXRTrackedController leftRemoteTracking_new, rightRemoteTracking_new;

static double s_lastUpdateMs = -1.0;
static bool   s_prevAimValid[WEBXR_HAND_COUNT];
static float  s_prevAimPos[WEBXR_HAND_COUNT][3];

/* xr-standard button indices (Quest Touch; webxr_input.h header comment) */
#define XRSTD_TRIGGER    0
#define XRSTD_SQUEEZE    1
#define XRSTD_THUMBSTICK 3
#define XRSTD_AX         4 /* A on right hand, X on left */
#define XRSTD_BY         5 /* B on right hand, Y on left */
#define XRSTD_THUMBREST  6
#define XRSTD_AXIS_X     2
#define XRSTD_AXIS_Y     3 /* +y = down in xr-standard; flipped below */

/* =====================================================================
 * Per-frame update
 * ===================================================================== */

static float ButtonValue(const WebXRControllerState *c, int i)
{
    return (i < c->buttonCount) ? c->buttons[i].value : 0.0f;
}
static bool ButtonPressed(const WebXRControllerState *c, int i)
{
    return i < c->buttonCount && c->buttons[i].pressed;
}
static bool ButtonTouched(const WebXRControllerState *c, int i)
{
    return i < c->buttonCount && c->buttons[i].touched;
}

/* Mirrors OpenXrInput.c:455-501 (button/axis mapping) with the xr-standard
 * gamepad as the source instead of XrActions. */
static void BuildHandState(int hand, const WebXRControllerState *raw,
                           WebXRRemoteState *st, WebXRTrackedController *tc,
                           float dtSeconds)
{
    memset(st, 0, sizeof(*st));

    if (raw->gamepadConnected)
    {
        const uint32_t thumbBit = (hand == WEBXR_HAND_LEFT) ? xrButton_LThumb : xrButton_RThumb;
        const uint32_t axBit    = (hand == WEBXR_HAND_LEFT) ? xrButton_X : xrButton_A;
        const uint32_t byBit    = (hand == WEBXR_HAND_LEFT) ? xrButton_Y : xrButton_B;

        /* triggers: analog value + the same >0.5 press threshold the fork used */
        st->IndexTrigger = ButtonValue(raw, XRSTD_TRIGGER);
        if (st->IndexTrigger > 0.5f)            st->Buttons |= xrButton_Trigger;
        if (ButtonTouched(raw, XRSTD_TRIGGER))  st->Touches |= xrButton_Trigger;

        st->GripTrigger = ButtonValue(raw, XRSTD_SQUEEZE);
        if (st->GripTrigger > 0.5f)             st->Buttons |= xrButton_GripTrigger;

        /* thumbstick click sets both the hand bit and xrButton_Joystick,
         * exactly like OpenXrInput.c:465-468/483-486 */
        if (ButtonPressed(raw, XRSTD_THUMBSTICK)) st->Buttons |= thumbBit | xrButton_Joystick;
        if (ButtonTouched(raw, XRSTD_THUMBSTICK)) st->Touches |= thumbBit | xrButton_Joystick;

        if (ButtonPressed(raw, XRSTD_AX))        st->Buttons |= axBit;
        if (ButtonTouched(raw, XRSTD_AX))        st->Touches |= axBit;
        if (ButtonPressed(raw, XRSTD_BY))        st->Buttons |= byBit;
        if (ButtonTouched(raw, XRSTD_BY))        st->Touches |= byBit;
        if (ButtonTouched(raw, XRSTD_THUMBREST)) st->Touches |= xrButton_ThumbRest;

        /* thumbstick: xr-standard axes[2,3]; y flipped to OpenXR convention
         * (+y = pushed forward/up) so ported flick/menu thresholds keep
         * their signs. Do NOT flip again in gameplay code. */
        if (raw->axisCount > XRSTD_AXIS_Y)
        {
            st->Joystick.x =  raw->axes[XRSTD_AXIS_X];
            st->Joystick.y = -raw->axes[XRSTD_AXIS_Y];
        }
    }

    /* tracking: aim pose (the fork used the OpenXR aim space, reports/06
     * §1.1); velocity derived from position deltas (no XrSpaceVelocity in
     * WebXR's gamepad path) */
    tc->Active = raw->present && raw->aimValid;
    tc->Pose.position.x    = raw->aimPosition[0];
    tc->Pose.position.y    = raw->aimPosition[1];
    tc->Pose.position.z    = raw->aimPosition[2];
    tc->Pose.orientation.x = raw->aimOrientation[0];
    tc->Pose.orientation.y = raw->aimOrientation[1];
    tc->Pose.orientation.z = raw->aimOrientation[2];
    tc->Pose.orientation.w = raw->aimOrientation[3];

    if (tc->Active && s_prevAimValid[hand] && dtSeconds > 0.0001f)
    {
        tc->Velocity.linearVelocity.x = (raw->aimPosition[0] - s_prevAimPos[hand][0]) / dtSeconds;
        tc->Velocity.linearVelocity.y = (raw->aimPosition[1] - s_prevAimPos[hand][1]) / dtSeconds;
        tc->Velocity.linearVelocity.z = (raw->aimPosition[2] - s_prevAimPos[hand][2]) / dtSeconds;
    }
    else
    {
        tc->Velocity.linearVelocity.x = 0.0f;
        tc->Velocity.linearVelocity.y = 0.0f;
        tc->Velocity.linearVelocity.z = 0.0f;
    }
    s_prevAimValid[hand] = tc->Active;
    memcpy(s_prevAimPos[hand], raw->aimPosition, sizeof(s_prevAimPos[hand]));
}

static void WebXRInput_ProcessHaptics(float frameMs);
static void WebXRInput_DebugTick(double nowMs);

void WebXRInput_Update(double nowMs)
{
    float frameMs = (s_lastUpdateMs >= 0.0) ? (float)(nowMs - s_lastUpdateMs) : 0.0f;
    if (frameMs > 1000.0f) frameMs = 1000.0f;
    s_lastUpdateMs = nowMs;

    webxr_get_controller_state(WEBXR_HAND_LEFT,  &webxr_controllers[WEBXR_HAND_LEFT]);
    webxr_get_controller_state(WEBXR_HAND_RIGHT, &webxr_controllers[WEBXR_HAND_RIGHT]);

    float dt = frameMs / 1000.0f;
    BuildHandState(WEBXR_HAND_LEFT,  &webxr_controllers[WEBXR_HAND_LEFT],
                   &leftTrackedRemoteState_new,  &leftRemoteTracking_new,  dt);
    BuildHandState(WEBXR_HAND_RIGHT, &webxr_controllers[WEBXR_HAND_RIGHT],
                   &rightTrackedRemoteState_new, &rightRemoteTracking_new, dt);

    WebXRInput_ProcessHaptics(frameMs);
    WebXRInput_DebugTick(nowMs);
}

void WebXRInput_Reset(void)
{
    memset(webxr_controllers, 0, sizeof(webxr_controllers));
    memset(&leftTrackedRemoteState_new,  0, sizeof(leftTrackedRemoteState_new));
    memset(&leftTrackedRemoteState_old,  0, sizeof(leftTrackedRemoteState_old));
    memset(&rightTrackedRemoteState_new, 0, sizeof(rightTrackedRemoteState_new));
    memset(&rightTrackedRemoteState_old, 0, sizeof(rightTrackedRemoteState_old));
    memset(&leftRemoteTracking_new,  0, sizeof(leftRemoteTracking_new));
    memset(&rightRemoteTracking_new, 0, sizeof(rightRemoteTracking_new));
    memset(s_prevAimValid, 0, sizeof(s_prevAimValid));
    s_lastUpdateMs = -1.0;
}

void WebXRInput_GetHMDPositionDelta(float out[3])
{
    void WebXRBridge_GetHMDPositionDelta(float out[3]); /* webxr_bridge.c */
    WebXRBridge_GetHMDPositionDelta(out);
}

/* =====================================================================
 * Haptics — TBXR_Vibrate-compatible channels (OpenXrInput.c:504-569).
 * Differences from the OpenXR original: instead of re-applying the haptic
 * every frame, a finite pulse is issued once for its full duration (the
 * browser actuator runs it out); the channel bookkeeping (busy-rejection,
 * countdown, -1 = continuous) is behavior-identical.
 * ===================================================================== */

static float s_vibDurationMs[WEBXR_HAND_COUNT]; /* >0 running, -1 continuous, 0 idle */
static float s_vibIntensity[WEBXR_HAND_COUNT];

bool WebXRInput_HapticPulse(int hand, float intensity, int durationMs)
{
    return webxr_haptic_pulse(hand, intensity, durationMs) != 0;
}

void WebXRInput_Vibrate(int durationMs, int channelMask, float intensity)
{
    for (int i = 0; i < WEBXR_HAND_COUNT; ++i)
    {
        if ((i + 1) & channelMask)
        {
            /* fork semantics: a busy channel rejects the whole request;
             * a continuous (-1) vibration is only cancelled by duration 0 */
            if (s_vibDurationMs[i] > 0.0f)
                return;
            if (s_vibDurationMs[i] == -1.0f && durationMs != 0)
                return;

            s_vibDurationMs[i] = (float)durationMs;
            s_vibIntensity[i]  = intensity;

            if (durationMs > 0)
                webxr_haptic_pulse(i, intensity, durationMs);
            else if (durationMs == 0)
                webxr_haptic_pulse(i, 0.0f, 1); /* cancel */
        }
    }
}

static void WebXRInput_ProcessHaptics(float frameMs)
{
    for (int i = 0; i < WEBXR_HAND_COUNT; ++i)
    {
        if (s_vibDurationMs[i] == -1.0f)
        {
            /* continuous: re-pulse every frame (pulse restarts the actuator) */
            webxr_haptic_pulse(i, s_vibIntensity[i], 100);
        }
        else if (s_vibDurationMs[i] > 0.0f)
        {
            s_vibDurationMs[i] -= frameMs;
            if (s_vibDurationMs[i] <= 0.0f)
            {
                s_vibDurationMs[i] = 0.0f;
                s_vibIntensity[i]  = 0.0f;
            }
        }
    }
}

/* =====================================================================
 * Debug dump — vr_inputdebug console command / ?inputdebug=1.
 * DOM overlay (headless tests + desktop mirror view read this) at ~5 Hz,
 * Con_Printf notify lines (visible inside the HMD) at 1 Hz.
 * ===================================================================== */

static int    s_debug = 0;
static double s_debugOverlayMs = 0.0;
static double s_debugConsoleMs = 0.0;

EM_JS(int, webxr_js_inputdebug_param, (void), {
    try {
        return new URLSearchParams(location.search).get('inputdebug') ? 1 : 0;
    } catch (e) { return 0; }
});

EM_JS(void, webxr_js_inputdebug_overlay, (const char *txt), {
    var el = document.getElementById('qq-inputdebug');
    if (!el) {
        el = document.createElement('pre');
        el.id = 'qq-inputdebug';
        el.style.cssText = 'position:fixed;top:0;right:0;z-index:1000;' +
            'background:rgba(0,0,0,.85);color:#9f9;font:11px/1.4 monospace;' +
            'padding:6px 8px;margin:0;pointer-events:none;white-space:pre;' +
            'text-align:left;max-width:60ch;';
        document.body.appendChild(el);
    }
    el.textContent = UTF8ToString(txt);
});

static int FormatHand(char *buf, int size, const char *tag,
                      const WebXRControllerState *raw,
                      const WebXRRemoteState *st,
                      const WebXRTrackedController *tc)
{
    if (!raw->present)
        return dpsnprintf(buf, size, "%s: absent\n", tag);

    int n = 0;
    n += dpsnprintf(buf + n, size - n,
        "%s: grip%c(%6.2f %6.2f %6.2f) aim%c(%6.2f %6.2f %6.2f)\n"
        "   aimQ(%5.2f %5.2f %5.2f %5.2f) vel(%5.2f %5.2f %5.2f)\n",
        tag,
        raw->gripValid ? ' ' : '!',
        raw->gripPosition[0], raw->gripPosition[1], raw->gripPosition[2],
        raw->aimValid ? ' ' : '!',
        raw->aimPosition[0], raw->aimPosition[1], raw->aimPosition[2],
        tc->Pose.orientation.x, tc->Pose.orientation.y,
        tc->Pose.orientation.z, tc->Pose.orientation.w,
        tc->Velocity.linearVelocity.x, tc->Velocity.linearVelocity.y,
        tc->Velocity.linearVelocity.z);
    n += dpsnprintf(buf + n, size - n,
        "   btn=%08x tch=%08x trig=%4.2f grip=%4.2f stick(%5.2f %5.2f)\n",
        st->Buttons, st->Touches, st->IndexTrigger, st->GripTrigger,
        st->Joystick.x, st->Joystick.y);
    n += dpsnprintf(buf + n, size - n,
        "   gp=%d hap=%d nb=%d na=%d raw[",
        raw->gamepadConnected, raw->hasHaptic, raw->buttonCount, raw->axisCount);
    for (int i = 0; i < raw->buttonCount && i < WEBXR_INPUT_MAX_BUTTONS; ++i)
        n += dpsnprintf(buf + n, size - n, "%s%c%.2f",
                        i ? " " : "",
                        raw->buttons[i].pressed ? 'P' : (raw->buttons[i].touched ? 't' : '.'),
                        raw->buttons[i].value);
    n += dpsnprintf(buf + n, size - n, "] axes[");
    for (int i = 0; i < raw->axisCount && i < WEBXR_INPUT_MAX_AXES; ++i)
        n += dpsnprintf(buf + n, size - n, "%s%5.2f", i ? " " : "", raw->axes[i]);
    n += dpsnprintf(buf + n, size - n, "]\n");
    return n;
}

static void WebXRInput_FormatState(char *buf, int size)
{
    float hmdDelta[3];
    WebXRInput_GetHMDPositionDelta(hmdDelta);

    int n = dpsnprintf(buf, size,
        "-- vr input (session=%d) --\n"
        "hmd pos(%6.2f %6.2f %6.2f) ypr(%6.1f %6.1f %6.1f)\n"
        "    delta(%6.3f %6.3f %6.3f)\n",
        WebXRBridge_IsSessionActive() ? 1 : 0,
        hmdPosition[0], hmdPosition[1], hmdPosition[2],
        hmdorientation[0], hmdorientation[1], hmdorientation[2],
        hmdDelta[0], hmdDelta[1], hmdDelta[2]);
    n += FormatHand(buf + n, size - n, "L",
                    &webxr_controllers[WEBXR_HAND_LEFT],
                    &leftTrackedRemoteState_new, &leftRemoteTracking_new);
    FormatHand(buf + n, size - n, "R",
               &webxr_controllers[WEBXR_HAND_RIGHT],
               &rightTrackedRemoteState_new, &rightRemoteTracking_new);
}

static void WebXRInput_DebugTick(double nowMs)
{
    if (!s_debug)
        return;

    char buf[1024];

    if (nowMs - s_debugOverlayMs > 200.0)
    {
        s_debugOverlayMs = nowMs;
        WebXRInput_FormatState(buf, sizeof(buf));
        webxr_js_inputdebug_overlay(buf);
    }
    if (nowMs - s_debugConsoleMs > 1000.0)
    {
        s_debugConsoleMs = nowMs;
        WebXRInput_FormatState(buf, sizeof(buf));
        Con_Printf("%s", buf);
    }
}

void WebXRInput_SetDebug(int enabled)
{
    s_debug = enabled;
    if (!enabled)
        webxr_js_inputdebug_overlay("");
}

static void WebXRInput_DebugCmd_f(void)
{
    int enable = (Cmd_Argc() > 1) ? atoi(Cmd_Argv(1)) : !s_debug;
    WebXRInput_SetDebug(enable);
    Con_Printf("vr_inputdebug %s\n", enable ? "ON (overlay + 1Hz notify)" : "off");
    if (enable && !WebXRBridge_IsSessionActive())
        Con_Printf("(no XR session active — controller state updates only in VR)\n");
}

void WebXRInput_Init(void)
{
    Cmd_AddCommand("vr_inputdebug", WebXRInput_DebugCmd_f,
                   "toggle live WebXR controller input state dump (overlay + console)");
    if (webxr_js_inputdebug_param())
        WebXRInput_SetDebug(1);
    printf("[webxr] input foundation initialised\n");
}
