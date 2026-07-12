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

void Host_SaveConfig(void);             /* host.c — writes binds + CVAR_SAVE cvars */

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
 * cl_trackingmode: runtime value vs saved preference (M4 stomp fix,
 * reports/08b open issue 3).
 *
 * Runtime rule (unchanged since M1/M3): flatscreen frames need 0 (classic
 * gun-follows-view — with 1 the viewmodel pins to the head because
 * weaponOffset stays zero), VR sessions run the user's chosen mode.
 * The bug: cl_trackingmode is CVAR_SAVE, so forcing it (boot 0 / session
 * start 1 / session end 0) leaked into config.cfg within one persist tick
 * and the user's 3DoF/6DoF choice from the VR options menu never survived.
 *
 * Separation: s_userTrackingMode is THE preference; s_forcedTrackingMode is
 * what this module last wrote. Any cvar value that differs from our last
 * write must have come from the user (VR options menu toggle, console,
 * test) and updates the preference. Config saves go through
 * IN_Weapon_SaveConfigPreservingTrackingMode(), which swaps the preference
 * in around Host_SaveConfig so the forced runtime value never persists.
 * ===================================================================== */
static int s_userTrackingMode   = -1;   /* -1 = not latched yet */
static int s_forcedTrackingMode = -1;   /* our last write; -1 = none */

static void TrackingMode_Force(int mode)
{
    if (cl_trackingmode.integer != mode)
        Cvar_SetValueQuick(&cl_trackingmode, (float)mode);
    s_forcedTrackingMode = mode;
}

void IN_Weapon_TrackingModeTick(void)
{
    if (s_userTrackingMode < 0)
        return; /* init hasn't latched the saved preference yet */
    if (cl_trackingmode.integer != s_forcedTrackingMode)
    {
        /* changed by something that isn't us -> new user preference */
        s_userTrackingMode = cl_trackingmode.integer;
        s_forcedTrackingMode = cl_trackingmode.integer;
    }
}

void IN_Weapon_SaveConfigPreservingTrackingMode(void)
{
    int runtime = cl_trackingmode.integer;
    IN_Weapon_TrackingModeTick(); /* catch a just-made user change first */
    if (s_userTrackingMode >= 0 && runtime != s_userTrackingMode)
    {
        Cvar_SetValueQuick(&cl_trackingmode, (float)s_userTrackingMode);
        Host_SaveConfig();
        Cvar_SetValueQuick(&cl_trackingmode, (float)runtime);
    }
    else
        Host_SaveConfig();
}

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

    /* Watch for user changes (VR options menu / console) BEFORE any forcing
     * below, so a live toggle updates the saved preference (M4 stomp fix). */
    IN_Weapon_TrackingModeTick();

    /* First frame of a session: flatscreen ran forced cl_trackingmode 0;
     * apply the USER'S saved mode for VR (fork default 1 = 6DoF; a user who
     * picked 3DoF in the VR options menu gets 3DoF back — the force-to-1
     * here used to stomp that, reports/08b issue 3). */
    if (!s_inSession)
    {
        s_inSession = true;
        TrackingMode_Force(s_userTrackingMode >= 0 ? s_userTrackingMode : 1);
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

    /* capture any in-session preference change made on the very last frames,
     * then back to the flatscreen operating point: 3DoF = classic
     * gun-follows-view. The user's mode is reapplied on the next session
     * start and is what gets saved to config.cfg (M4 stomp fix). */
    IN_Weapon_TrackingModeTick();
    TrackingMode_Force(0);
}

void IN_Weapon_Init(void)
{
    /* The fork shipped these binds in its config.cfg asset
     * (reports/06 §4 "Weapon switching"); the shareware pak has neither. */
    Cbuf_AddText("bind / \"impulse 10\"\n"
                 "bind # \"impulse 12\"\n");

    /* M4 stomp fix: config.cfg was exec'd synchronously inside Host_Main()
     * (host.c:1317-1318), so the cvar currently holds the user's SAVED
     * preference — latch it before forcing the flatscreen runtime mode.
     * (This force lived in main_web.c's Cbuf config block before, where it
     * both raced the latch and looked like a user change; it is owned here
     * now.) */
    s_userTrackingMode = cl_trackingmode.integer;
    TrackingMode_Force(0);

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
    case 28: return (double)s_userTrackingMode; /* saved preference (M4) */
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
