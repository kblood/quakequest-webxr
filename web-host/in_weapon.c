/*
 * in_weapon.c — M3 chunk 2: VR weapon aim, firing & haptics (WEBXR-PORT M3-weapon).
 *
 * See in_weapon.h for scope. Logic is ported line-for-line from
 * QuakeQuestSrc/QuakeQuest_OpenXR.c (referenced per block below); only the
 * data source changed: the M3 foundation's TBXR-compatible globals
 * (webxr_input.h) replace the OpenXR reads, WebXRInput_Vibrate replaces
 * TBXR_Vibrate, WebXRBridge_QuatToYawPitchRoll (ported in M2) replaces
 * TBXR_Common.c's QuatToYawPitchRoll, emscripten_get_now() replaces
 * TBXR_GetTimeInMilliSeconds().
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <math.h>

#include <emscripten.h>

#include "quakedef.h"
#include "keys.h"

#include "webxr_input.h"
#include "webxr_bridge.h"
#include "in_weapon.h"

/* ---- engine entry points / externs (darkplaces side) ---- */
void QC_KeyEvent(int state, int key, int character); /* vid_android.c -> Key_Event */
extern float gunangles[3];              /* view.c — consumed by view.c + cl_input.c:1845 */
extern float gunorg[3];                 /* view.c — final gun origin (probe only) */
extern cvar_t cl_righthanded;           /* cl_input.c:446 — dominant-hand select */
extern cvar_t vr_weaponpitchadjust;     /* cl_input.c:447 — pitch trim baked into gunangles */
extern cvar_t cl_trackingmode;          /* cl_input.c:448 — 0 = 3DoF, 1 = 6DoF */
extern cvar_t r_lasersight;             /* gl_rmain.c:129 — 0/1/2 beam mode */
extern cvar_t vr_worldscale;            /* gl_rmain.c:52 — Quake units per meter (probe only) */

/* ---- host globals (main_web.c / webxr_bridge.c) ---- */
qboolean VR_UseScreenLayer(void);       /* main_web.c — true when flat/2D layout */
extern float hmdorientation[3];         /* [pitch,yaw,roll] degrees */
extern float hmdPosition[3];            /* raw XR-space head position (meters) */
extern float weaponOffset[3];           /* main_web.c; consumed by view.c:975-977 (which
                                         * applies the vr_worldscale meters->Quake-units
                                         * scaling and the XR->Quake axis swap itself —
                                         * this module writes raw XR-space METERS) */

/* ---- fork globals this module now owns (QuakeQuest_OpenXR.c:50-51) ---- */
float weaponVelocity[3];
bool weapon_stabilised = false; /* fork: qboolean; bool here so the engine-free
                                 * header (in_weapon.h) can declare it */

/* ---- private state ---- */
/* PRIVATE previous-state copies for edge detection — deliberately NOT the
 * shared *TrackedRemoteState_old globals: four M3 chunks run from the same
 * dispatch and a shared new->old copy would erase the other chunks' edges. */
static WebXRRemoteState s_prevLeft, s_prevRight;
static bool   s_aimActive = false;     /* controller drove gunangles this frame */
static bool   s_inSession = false;     /* tracks session-start transition */
static double s_nowMs = 0.0;           /* TBXR_GetTimeInMilliSeconds() stand-in */

/* haptic evidence counters (test probe) */
static int    s_hapticFireCount = 0;
static float  s_hapticLastLevel = 0.0f;
static int    s_hapticLastChannel = 0;

/* =====================================================================
 * Helpers ported verbatim
 * ===================================================================== */

/* QuakeQuest_OpenXR.c:609-615 — but against a caller-supplied previous
 * state instead of the shared *_old globals (see note above). */
static void handleTrackedControllerButton(const WebXRRemoteState *trackedRemoteState,
                                          const WebXRRemoteState *prevTrackedRemoteState,
                                          uint32_t button, int key)
{
    if ((trackedRemoteState->Buttons & button) != (prevTrackedRemoteState->Buttons & button))
    {
        QC_KeyEvent((trackedRemoteState->Buttons & button) > 0 ? 1 : 0, key, 0);
    }
}

/* QuakeQuest_OpenXR.c:617-630 — pure math, copied verbatim. */
static void rotateAboutOrigin(float v1, float v2, float rotation, vec2_t out)
{
    vec3_t temp;
    temp[0] = v1;
    temp[1] = v2;
    temp[2] = 0;

    vec3_t v;
    matrix4x4_t matrix;
    Matrix4x4_CreateFromQuakeEntity(&matrix, 0.0f, 0.0f, 0.0f, 0.0f, rotation, 0.0f, 1.0f);
    Matrix4x4_Transform(&matrix, temp, v);

    out[0] = v[0];
    out[1] = v[1];
}

/* QuakeQuest_OpenXR.c:600-603 */
static float length2d(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

/* =====================================================================
 * VR_HapticEvent — ported from QuakeQuest_OpenXR.c:369-467.
 * Per-weapon rumble pattern while the fire trigger is held; channel = both
 * hands when weapon-stabilised, else the dominant hand. The fork calls this
 * every frame with null params (its main loop, QuakeQuest_OpenXR.c:307) and
 * the function does its own interval bookkeeping — same here, called from
 * IN_Weapon_Update. NOTE: this fork has NO damage-event haptics; firing is
 * the only rumble source (reports/06 §4).
 * ===================================================================== */
void VR_HapticEvent(const char *event, int position, int flags,
                    int intensity, float angle, float yHeight)
{
    (void)event; (void)position; (void)flags; (void)intensity;
    (void)angle; (void)yHeight;

    if (VR_UseScreenLayer())
    {
        return;
    }

    qboolean isFirePressed = false;
    if (cl_righthanded.integer) {
        //Fire
        isFirePressed = (rightTrackedRemoteState_new.Buttons & xrButton_Trigger) != 0;
    } else {
        isFirePressed = (leftTrackedRemoteState_new.Buttons & xrButton_Trigger) != 0;
    }

    if (isFirePressed)
    {
        static double timeLastHaptic = 0;
        double timeNow = s_nowMs;

        float hapticInterval = 0;
        float hapticLevel = 0;
        float hapticLength = 0;

        switch (cl.stats[STAT_ACTIVEWEAPON])
        {
            case IT_SHOTGUN:
            {
                hapticInterval = 500;
                hapticLevel = 0.7f;
                hapticLength = 150;
            }
            break;
            case IT_SUPER_SHOTGUN:
            {
                hapticInterval = 700;
                hapticLevel = 0.8f;
                hapticLength = 200;
            }
                break;
            case IT_NAILGUN:
            {
                hapticInterval = 100;
                hapticLevel = 0.6f;
                hapticLength = 50;
            }
                break;
            case IT_SUPER_NAILGUN:
            {
                hapticInterval = 80;
                hapticLevel = 0.9f;
                hapticLength = 50;
            }
                break;
            case IT_GRENADE_LAUNCHER:
            {
                hapticInterval = 600;
                hapticLevel = 0.7f;
                hapticLength = 100;
            }
                break;
            case IT_ROCKET_LAUNCHER:
            {
                hapticInterval = 800;
                hapticLevel = 1.0f;
                hapticLength = 300;
            }
                break;
            case IT_LIGHTNING:
            {
                hapticInterval = 100;
                hapticLevel = lhrandom(0.0, 0.8f);
                hapticLength = 80;
            }
                break;
            case IT_SUPER_LIGHTNING:
            {
                hapticInterval = 100;
                hapticLevel = lhrandom(0.3, 1.0f);
                hapticLength = 60;
            }
                break;
            case IT_AXE:
            {
                hapticInterval = 500;
                hapticLevel = 0.6f;
                hapticLength = 100;
            }
                break;
        }

        if ((timeNow - timeLastHaptic) > hapticInterval)
        {
            timeLastHaptic = timeNow;
            int channel = weapon_stabilised ? 3 : (cl_righthanded.integer ? 2 : 1);
            WebXRInput_Vibrate(hapticLength, channel, hapticLevel);

            /* test-probe evidence (not in the fork) */
            s_hapticFireCount++;
            s_hapticLastLevel = hapticLevel;
            s_hapticLastChannel = channel;
        }
    }
}

/* =====================================================================
 * Per-frame update — the weapon-relevant subset of HandleInput_Default()
 * (QuakeQuest_OpenXR.c:632-1023). Locomotion / menus / comfort belong to
 * the sibling M3 chunks and are NOT handled here.
 * ===================================================================== */

bool IN_Weapon_AimActive(void)
{
    return s_aimActive;
}

void IN_Weapon_Update(double nowMs)
{
    s_nowMs = nowMs;
    s_aimActive = false;

    if (!WebXRBridge_IsSessionActive())
        return;

    /* First frame of a session: the weapon decoupling below needs the
     * engine's 6DoF weapon-matrix path (view.c:965-991). Flatscreen boot
     * forces cl_trackingmode 0 (main_web.c); switch to the fork's VR default
     * once per session — the VR options menu can still toggle it live. */
    if (!s_inSession)
    {
        s_inSession = true;
        if (cl_trackingmode.integer != 1)
            Cvar_SetValueQuick(&cl_trackingmode, 1);
    }

    /* QuakeQuest_OpenXR.c:640-648 — yaw offset + dominant-hand selection */
    float yawOffset = cl.viewangles[YAW] - hmdorientation[YAW];

    WebXRRemoteState *dominantTrackedRemoteState = cl_righthanded.integer ? &rightTrackedRemoteState_new : &leftTrackedRemoteState_new;
    WebXRRemoteState *dominantTrackedRemoteStateOld = cl_righthanded.integer ? &s_prevRight : &s_prevLeft;
    WebXRTrackedController *dominantRemoteTracking = cl_righthanded.integer ? &rightRemoteTracking_new : &leftRemoteTracking_new;
    WebXRRemoteState *offHandTrackedRemoteState = !cl_righthanded.integer ? &rightTrackedRemoteState_new : &leftTrackedRemoteState_new;
    WebXRTrackedController *offHandRemoteTracking = !cl_righthanded.integer ? &rightRemoteTracking_new : &leftRemoteTracking_new;

    /* QuakeQuest_OpenXR.c:747-754 — two-handed stabilization condition */
    float distance = sqrtf(powf(offHandRemoteTracking->Pose.position.x - dominantRemoteTracking->Pose.position.x, 2) +
                           powf(offHandRemoteTracking->Pose.position.y - dominantRemoteTracking->Pose.position.y, 2) +
                           powf(offHandRemoteTracking->Pose.position.z - dominantRemoteTracking->Pose.position.z, 2));

    weapon_stabilised = distance < 0.5f &&
            (offHandTrackedRemoteState->Buttons & xrButton_GripTrigger) &&
            cl.stats[STAT_ACTIVEWEAPON] != IT_AXE;

    /* QuakeQuest_OpenXR.c:756-788 — dominant-hand aim block. Gated on a
     * located aim pose (the OpenXR original always ran; a lost WebXR pose
     * reads zeros and would snap the gun into the head). */
    if (dominantRemoteTracking->Active)
    {
        weaponOffset[0] = dominantRemoteTracking->Pose.position.x - hmdPosition[0];
        weaponOffset[1] = dominantRemoteTracking->Pose.position.y - hmdPosition[1];
        weaponOffset[2] = dominantRemoteTracking->Pose.position.z - hmdPosition[2];

        weaponVelocity[0] = dominantRemoteTracking->Velocity.linearVelocity.x;
        weaponVelocity[1] = dominantRemoteTracking->Velocity.linearVelocity.y;
        weaponVelocity[2] = dominantRemoteTracking->Velocity.linearVelocity.z;

        ///Weapon location relative to view
        vec2_t v;
        rotateAboutOrigin(weaponOffset[0], weaponOffset[2], -yawOffset, v);
        weaponOffset[0] = v[0];
        weaponOffset[2] = v[1];

        //Set gun angles
        const float quatRemote[4] = { dominantRemoteTracking->Pose.orientation.x,
                                      dominantRemoteTracking->Pose.orientation.y,
                                      dominantRemoteTracking->Pose.orientation.z,
                                      dominantRemoteTracking->Pose.orientation.w };
        vec3_t rotation = {vr_weaponpitchadjust.value, 0, 0};
        WebXRBridge_QuatToYawPitchRoll(quatRemote, rotation, gunangles);

        if (weapon_stabilised)
        {
            float z = offHandRemoteTracking->Pose.position.z - dominantRemoteTracking->Pose.position.z;
            float x = offHandRemoteTracking->Pose.position.x - dominantRemoteTracking->Pose.position.x;
            float y = offHandRemoteTracking->Pose.position.y - dominantRemoteTracking->Pose.position.y;
            float zxDist = length2d(x, z);

            if (zxDist != 0.0f && z != 0.0f) {
                VectorSet(gunangles, -RAD2DEG(atanf(y / zxDist)), -RAD2DEG(atan2f(x, -z)), gunangles[ROLL]);
            }
        }

        gunangles[YAW] += yawOffset;

        s_aimActive = true;
    }

    /* QuakeQuest_OpenXR.c:790-795 — laser-sight cycle on dominant
     * thumbstick click */
    if ((dominantTrackedRemoteState->Buttons & xrButton_Joystick) &&
        (dominantTrackedRemoteState->Buttons & xrButton_Joystick) !=
        (dominantTrackedRemoteStateOld->Buttons & xrButton_Joystick)) {
        Cvar_SetValueQuick(&r_lasersight, (r_lasersight.integer + 1) % 3);
    }

    /* QuakeQuest_OpenXR.c:876-886 — weapon-cycle flick on the RIGHT stick
     * (the fork uses the right stick regardless of handedness; the big-screen
     * d-pad branch is chunk 3's). Keys '/' and '#' are bound to impulse 10/12
     * in IN_Weapon_Init, mirroring the fork's shipped config.cfg. */
    if (!VR_UseScreenLayer())
    {
        //Weapon/Inventory Chooser
        int rightJoyState = (rightTrackedRemoteState_new.Joystick.y < -0.7f ? 1 : 0);
        if (rightJoyState != (s_prevRight.Joystick.y < -0.7f ? 1 : 0)) {
            QC_KeyEvent(rightJoyState, '/', 0);
        }
        rightJoyState = (rightTrackedRemoteState_new.Joystick.y > 0.7f ? 1 : 0);
        if (rightJoyState != (s_prevRight.Joystick.y > 0.7f ? 1 : 0)) {
            QC_KeyEvent(rightJoyState, '#', 0);
        }
    }

    /* QuakeQuest_OpenXR.c:889-899/958-963 — dominant trigger = fire
     * (K_MOUSE1, bound to +attack). The other trigger's run-modifier
     * (K_SHIFT) belongs to chunk 1. */
    handleTrackedControllerButton(dominantTrackedRemoteState,
                                  dominantTrackedRemoteStateOld,
                                  xrButton_Trigger, K_MOUSE1);

    /* fork main loop called this once per frame (QuakeQuest_OpenXR.c:307) */
    VR_HapticEvent(NULL, 0, 0, 0, 0, 0);

    /* save state — private copies (see top-of-file note) */
    s_prevLeft = leftTrackedRemoteState_new;
    s_prevRight = rightTrackedRemoteState_new;
}

void IN_Weapon_SessionEnd(void)
{
    /* release anything we might be holding down (trigger held across an
     * exit would latch +attack into flatscreen) */
    QC_KeyEvent(0, K_MOUSE1, 0);
    QC_KeyEvent(0, '/', 0);
    QC_KeyEvent(0, '#', 0);

    memset(&s_prevLeft, 0, sizeof(s_prevLeft));
    memset(&s_prevRight, 0, sizeof(s_prevRight));
    weapon_stabilised = false;
    VectorClear(weaponVelocity);
    s_aimActive = false;
    s_inSession = false;

    /* back to the flatscreen operating point (main_web.c forces this only
     * at boot): 3DoF = classic gun-follows-view */
    if (cl_trackingmode.integer != 0)
        Cvar_SetValueQuick(&cl_trackingmode, 0);
}

void IN_Weapon_Init(void)
{
    /* The fork shipped these binds in its config.cfg asset
     * (reports/06 §4 "Weapon switching"); the shareware pak has neither. */
    Cbuf_AddText("bind / \"impulse 10\"\n"
                 "bind # \"impulse 12\"\n");
    Con_Printf("[webxr] weapon input initialised (M3-weapon)\n");
}

/* =====================================================================
 * Test probes (EMSCRIPTEN_KEEPALIVE; used by web-host/test/m3-weapon-test.mjs)
 * ===================================================================== */

EMSCRIPTEN_KEEPALIVE
double IN_Weapon_Probe(int what)
{
    switch (what)
    {
    case 0: case 1: case 2:    return gunangles[what];          /* pitch,yaw,roll */
    case 3: case 4: case 5:    return weaponOffset[what - 3];   /* XR meters */
    case 6: case 7: case 8:    return gunorg[what - 6];         /* Quake units */
    case 9: case 10: case 11:  return cl.csqc_vieworiginfromengine[what - 9]; /* vieworg */
    case 12: return (double)cl.stats[STAT_ACTIVEWEAPON];
    case 13: return (double)cl.stats[STAT_SHELLS];
    case 14: return (double)cl.stats[STAT_HEALTH];
    case 15: return (double)cls.signon;
    case 16: return weapon_stabilised ? 1.0 : 0.0;
    case 17: return (double)s_hapticFireCount;
    case 18: return (double)s_hapticLastLevel;
    case 19: return (double)cl_trackingmode.integer;
    case 20: return s_aimActive ? 1.0 : 0.0;
    case 21: return (double)r_lasersight.integer;
    case 22: return (double)vr_worldscale.value;
    case 23: return (double)s_hapticLastChannel;
    case 24: return (double)cl_righthanded.integer;
    case 25: case 26: case 27: return hmdPosition[what - 25]; /* XR meters */
    default: return -99999.0;
    }
}

/* Test-harness cvar setter (HEAPU8 isn't exported, so no JS string
 * marshaling): what uses the same indices as IN_Weapon_Probe where
 * applicable. Returns false for unknown targets. */
EMSCRIPTEN_KEEPALIVE
int IN_Weapon_TestSet(int what, double value)
{
    switch (what)
    {
    case 24: Cvar_SetValueQuick(&cl_righthanded, (float)value); return 1;
    case 21: Cvar_SetValueQuick(&r_lasersight, (float)value);   return 1;
    case 19: Cvar_SetValueQuick(&cl_trackingmode, (float)value); return 1;
    default: return 0;
    }
}
