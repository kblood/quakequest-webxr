/*
 * webxr_bridge.c — WebXR session/rendering bridge (Milestone 2).
 *
 * Reproduces the OpenXR host's frame protocol (QuakeQuest_OpenXR.c's
 * AppThreadFunction loop) on top of the vendored+patched emscripten-webxr
 * library: the browser's XRSession.requestAnimationFrame drives
 * QC_BeginFrame / (per-eye QC_DrawFrame) / QC_EndFrame while the flatscreen
 * emscripten main loop is paused.
 *
 * Key structural difference from OpenXR (design doc §5): WebXR gives ONE
 * shared framebuffer with per-eye sub-rect viewports instead of two per-eye
 * FBOs at (0,0). The per-eye offset rides the existing QC_DrawFrame(eye,x,y)
 * seam — SCR_DrawScreen assigns x,y to r_refdef.view.x/y, which places both
 * the 3D view (R_Viewport_InitPerspective) and the 2D/HUD stage
 * (R_ResetViewRendering2D's ortho viewport, gl_rmain.c:5685) inside the
 * shared target. The Android host always passed (0,0) because each OpenXR
 * eye buffer started at the origin; the parameters exist for exactly this.
 *
 * Framebuffer plumbing (design doc §5, validated by fbo_smoketest.c):
 * library_webxr.js raw-binds XRWebGLLayer.framebuffer (registered in
 * GL.framebuffers with .name=id) before invoking OnXRFrame; the engine's
 * R_Mesh_Start() then re-fetches GL_FRAMEBUFFER_BINDING into
 * gl_state.defaultframebufferobject each QC_DrawFrame, so engine-internal
 * FBO passes (shadowmaps etc.) return to the XR layer framebuffer.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>

#include "lib/webxr/webxr.h"
#include "webxr_bridge.h"
#include "webxr_input.h"   /* M3 foundation: controller snapshot + haptics */
#include "in_weapon.h"     /* WEBXR-PORT M3-weapon: IN_Weapon_AimActive */
#include "in_menu.h"       /* WEBXR-PORT M3-hud: menu toggle + nav keys */
#include "vr_menu_quad.h"  /* WEBXR-PORT M3-hud: world-anchored 2D-UI quad */
#include "in_locomotion.h" /* WEBXR-PORT M3-loco: chunk 1 (locomotion/turning) */
#include "in_comfort.h"    /* WEBXR-PORT M3-comfort: recenter/bullet-time */

/* ---- engine entry points / externs (darkplaces side) ---- */
void QC_BeginFrame(bool stopTime);            /* vid_android.c */
void QC_DrawFrame(int eye, int x, int y);     /* vid_android.c */
void QC_EndFrame(void);                       /* vid_android.c */
void QC_MoveEvent(float yaw, float pitch, float roll); /* vid_android.c */
void QC_SetResolution(int width, int height); /* vid_android.c -> VID_Restart_f */
extern int andrw, andrh;                      /* vid_android.c — current engine render size */
extern float gunangles[3];                    /* view.c — weapon aim; M2 aims with the head */
extern int vrMode;                            /* menu.c — HUD/menu VR layout switch */

/* ---- host globals shared with main_web.c (M1 defines them) ---- */
extern float hmdorientation[3];  /* [pitch,yaw,roll] degrees */
extern float hmdPosition[3];     /* raw XR-space head position (meters) */
extern float playerHeight;       /* standing-height reference (XR y at VR entry) */

bool VR_UseScreenLayer(void);    /* main_web.c — true when flat/2D layout */
void WebHost_PersistTick(void);  /* main_web.c — config/save IDBFS persistence */

/* =====================================================================
 * Bridge state
 * ===================================================================== */
#define WEBXR_EYES 2

static bool  s_sessionActive = false;  /* between OnSessionStart and OnSessionEnd */
static bool  s_frameDataValid = false; /* projections/viewports cached from an XR frame */
static bool  s_inXRFrame = false;      /* inside OnXRFrame's engine pump */
static bool  s_needResolution = false; /* first-XR-frame QC_SetResolution pending */
static float s_eyeProjection[WEBXR_EYES][16];
static int   s_eyeViewport[WEBXR_EYES][4]; /* x, y, w, h */
static int   s_badViewCountLogged = 0;
static double s_lastXRFrameMs = 0.0;   /* emscripten_get_now() of last OnXRFrame (bug-3 self-heal) */

/* depth params pushed to the XR session (baked into XRView.projectionMatrix).
 * Engine units (Quake units), NOT meters — the matrix math is unitless and
 * the engine's view space is Quake-scaled (design doc §4.1). */
static bool  s_projParamsSet = false;
static float s_depthNear = 0.0f;
static float s_depthFar = 0.0f;

/* host-side pose bookkeeping ported from QuakeQuest_OpenXR.c (M3 consumers:
 * locomotion uses the per-frame position delta; playerYaw feeds stick turn). */
static float s_playerYaw = -999.0f;
static float s_worldPosition[3];
static float s_positionDelta[3];
static bool  s_useScreenPrev = true;   /* we arrive from flatscreen = screen layer */

/* =====================================================================
 * JS helpers
 * ===================================================================== */

/* Re-bind the XR layer framebuffer raw (same call library_webxr.js makes
 * before OnXRFrame). Needed again per-eye because the first-frame
 * QC_SetResolution -> VID_Restart_f path rebinds framebuffers while
 * restarting the renderer. */
EM_JS(void, webxr_js_bind_layer_fbo, (void), {
    var s = Module['webxr_session'];
    if (!s || !s.renderState.baseLayer) return;
    var fb = s.renderState.baseLayer.framebuffer;
    if (fb) GLctx.bindFramebuffer(GLctx.FRAMEBUFFER, fb);
});

/* Unbind back to the canvas backbuffer (session end). */
EM_JS(void, webxr_js_bind_canvas, (void), {
    GLctx.bindFramebuffer(GLctx.FRAMEBUFFER, null);
});

/* Surface session state to the page UI (button labels etc.). */
EM_JS(void, webxr_js_notify_state, (int active), {
    if (typeof window !== 'undefined' && window.onQuakeXRState)
        window.onQuakeXRState(!!active);
});

/* WEBXR-PORT bug-3 self-heal: JS-side truth about the session object.
 * library_webxr.js nulls Module['webxr_session'] in its 'end' listener;
 * if that ran but the C end-callback somehow didn't (an exception in the
 * teardown chain), the C flag is stale and must be reconciled. */
EM_JS(int, webxr_js_session_exists, (void), {
    return (typeof Module !== 'undefined' && Module['webxr_session']) ? 1 : 0;
});

/* WEBXR-PORT bug-3 self-heal: drop a stale (ended but never cleaned-up) JS
 * session reference so a fresh request starts from consistent state. */
EM_JS(void, webxr_js_drop_session, (void), {
    if (typeof Module !== 'undefined') Module['webxr_session'] = null;
});

/* =====================================================================
 * QuatToYawPitchRoll — ported from QuakeQuestSrc/TBXR_Common.c:734-938.
 * Types changed from XrQuaternionf/XrVector4f/ovrMatrix4f to plain float
 * arrays; the algorithm (incl. the -z/-x/y XR->Quake axis remap) is
 * byte-for-byte the same math. WebXR uses the identical right-handed,
 * Y-up, -Z-forward convention as OpenXR, so no new conversion is needed.
 * ===================================================================== */

#define BRIDGE_EPSILON 0.001f
#define BRIDGE_DEG2RAD(a) ((a) * (float)M_PI / 180.0f)

typedef float mat4_rows[4][4];

static void Bridge_MatFromQuat(mat4_rows out, const float q[4]) /* q = x,y,z,w */
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float ww = w*w, xx = x*x, yy = y*y, zz = z*z;

    out[0][0] = ww + xx - yy - zz;
    out[0][1] = 2 * (x*y - w*z);
    out[0][2] = 2 * (x*z + w*y);
    out[0][3] = 0;
    out[1][0] = 2 * (x*y + w*z);
    out[1][1] = ww - xx + yy - zz;
    out[1][2] = 2 * (y*z - w*x);
    out[1][3] = 0;
    out[2][0] = 2 * (x*z - w*y);
    out[2][1] = 2 * (y*z + w*x);
    out[2][2] = ww - xx - yy + zz;
    out[2][3] = 0;
    out[3][0] = 0; out[3][1] = 0; out[3][2] = 0; out[3][3] = 1;
}

static void Bridge_MatMultiply(mat4_rows out, const mat4_rows a, const mat4_rows b)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            out[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j] + a[i][3]*b[3][j];
}

static void Bridge_MatCreateRotation(mat4_rows out, float radX, float radY, float radZ)
{
    const float sinX = sinf(radX), cosX = cosf(radX);
    const mat4_rows rotX = {{1,0,0,0},{0,cosX,-sinX,0},{0,sinX,cosX,0},{0,0,0,1}};
    const float sinY = sinf(radY), cosY = cosf(radY);
    const mat4_rows rotY = {{cosY,0,sinY,0},{0,1,0,0},{-sinY,0,cosY,0},{0,0,0,1}};
    const float sinZ = sinf(radZ), cosZ = cosf(radZ);
    const mat4_rows rotZ = {{cosZ,-sinZ,0,0},{sinZ,cosZ,0,0},{0,0,1,0},{0,0,0,1}};
    mat4_rows rotXY;
    Bridge_MatMultiply(rotXY, rotY, rotX);
    Bridge_MatMultiply(out, rotZ, rotXY);
}

static void Bridge_MatTransformVec4(float out[4], const mat4_rows a, const float v[4])
{
    int i;
    for (i = 0; i < 4; i++)
        out[i] = a[i][0]*v[0] + a[i][1]*v[1] + a[i][2]*v[2] + a[i][3]*v[3];
}

static void Bridge_NormalizeVec3(float v[3])
{
    float invLength = 1.0f / sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    v[0] *= invLength; v[1] *= invLength; v[2] *= invLength;
}

static void Bridge_NormalizeAngles(float angles[3])
{
    while (angles[0] >= 90) angles[0] -= 180;
    while (angles[1] >= 180) angles[1] -= 360;
    while (angles[2] >= 180) angles[2] -= 360;
    while (angles[0] < -90) angles[0] += 180;
    while (angles[1] < -180) angles[1] += 360;
    while (angles[2] < -180) angles[2] += 360;
}

static void Bridge_GetAnglesFromVectors(const float forward[3], const float right[3], const float up[3], float angles[3])
{
    float sr, sp, sy, cr, cp, cy;

    sp = -forward[2];

    float cp_x_cy = forward[0];
    float cp_x_sy = forward[1];
    float cp_x_sr = -right[2];
    float cp_x_cr = up[2];

    float yaw = atan2f(cp_x_sy, cp_x_cy);
    float roll = atan2f(cp_x_sr, cp_x_cr);

    cy = cosf(yaw);
    sy = sinf(yaw);
    cr = cosf(roll);
    sr = sinf(roll);

    if (fabsf(cy) > BRIDGE_EPSILON)
        cp = cp_x_cy / cy;
    else if (fabsf(sy) > BRIDGE_EPSILON)
        cp = cp_x_sy / sy;
    else if (fabsf(sr) > BRIDGE_EPSILON)
        cp = cp_x_sr / sr;
    else if (fabsf(cr) > BRIDGE_EPSILON)
        cp = cp_x_cr / cr;
    else
        cp = cosf(asinf(sp));

    float pitch = atan2f(sp, cp);

    angles[0] = pitch / ((float)M_PI * 2.f / 360.f);
    angles[1] = yaw / ((float)M_PI * 2.f / 360.f);
    angles[2] = roll / ((float)M_PI * 2.f / 360.f);

    Bridge_NormalizeAngles(angles);
}

void WebXRBridge_QuatToYawPitchRoll(const float q[4], const float rotationAdjust[3], float out[3])
{
    mat4_rows mat;
    Bridge_MatFromQuat(mat, q);

    if (rotationAdjust && (rotationAdjust[0] != 0.0f || rotationAdjust[1] != 0.0f || rotationAdjust[2] != 0.0f))
    {
        mat4_rows rot, tmp;
        Bridge_MatCreateRotation(rot, BRIDGE_DEG2RAD(rotationAdjust[0]),
                                 BRIDGE_DEG2RAD(rotationAdjust[1]),
                                 BRIDGE_DEG2RAD(rotationAdjust[2]));
        memcpy(tmp, mat, sizeof(tmp));
        Bridge_MatMultiply(mat, tmp, rot);
    }

    /* forward, right, up in XR space (right-handed, Y-up, -Z-forward) */
    const float v1[4] = {0, 0, -1, 0};
    const float v2[4] = {1, 0, 0, 0};
    const float v3[4] = {0, 1, 0, 0};
    float fwd4[4], right4[4], up4[4];
    Bridge_MatTransformVec4(fwd4, mat, v1);
    Bridge_MatTransformVec4(right4, mat, v2);
    Bridge_MatTransformVec4(up4, mat, v3);

    /* XR -> Quake axis remap: quakeX = -xr.z, quakeY = -xr.x, quakeZ = xr.y */
    float forward[3] = {-fwd4[2],   -fwd4[0],   fwd4[1]};
    float right[3]   = {-right4[2], -right4[0], right4[1]};
    float up[3]      = {-up4[2],    -up4[0],    up4[1]};

    Bridge_NormalizeVec3(forward);
    Bridge_NormalizeVec3(right);
    Bridge_NormalizeVec3(up);

    Bridge_GetAnglesFromVectors(forward, right, up, out);
}

/* =====================================================================
 * VR_* engine-facing symbols this module owns (design doc §6)
 * ===================================================================== */

/* gl_backend.c:960,1004 calls this unconditionally every frame; the bool
 * return is ignored — only mutating `projection` has any effect. Flat: leave
 * the engine's own frustum matrix untouched. XR frame: overwrite with the
 * cached XRView.projectionMatrix (same GL column-major layout, direct copy). */
bool VR_GetVRProjection(int eye, float zNear, float zFar, float *projection)
{
    if (!s_inXRFrame || !s_frameDataValid || eye < 0 || eye >= WEBXR_EYES)
        return false;

    /* Keep the session's depth params tracking the engine's (Quake-unit)
     * clip planes so the browser bakes compatible near/far into
     * projectionMatrix. farclip fluctuates per frame (scene-bounds derived)
     * and R_Viewport_InitPerspectiveInfinite passes 1<<23, so quantize far
     * up to the next power of two (capped) to make updates rare. */
    float farq = zFar > 65536.0f ? 65536.0f : zFar;
    float p2 = 1024.0f;
    while (p2 < farq) p2 *= 2.0f;
    if (!s_projParamsSet || fabsf(zNear - s_depthNear) > 0.01f || p2 != s_depthFar)
    {
        webxr_set_projection_params(zNear, p2);
        s_depthNear = zNear;
        s_depthFar = p2;
        s_projParamsSet = true;
    }

    memcpy(projection, s_eyeProjection[eye], 16 * sizeof(float));
    return true;
}

/* Ported from QuakeQuest_OpenXR.c:185-190 (types unchanged, playerYaw kept
 * host-side for M3 locomotion). */
void VR_SetHMDOrientation(float pitch, float yaw, float roll)
{
    hmdorientation[0] = pitch;
    hmdorientation[1] = yaw;
    hmdorientation[2] = roll;

    if (!VR_UseScreenLayer() || s_playerYaw == -999.0f)
        s_playerYaw = yaw;
}

/* Ported from QuakeQuest_OpenXR.c:195-216. Position is stored RAW in
 * XR axis order (x=right, y=up, z=backward) — consumers in view.c remap
 * per-field at the read site (design doc §4.3); do NOT remap here.
 *
 * WEBXR-PORT headset-QA round 2 (duck): the artificial-crouch eye offset
 * (in_locomotion.c, right-B hold) is subtracted from the reported Y HERE,
 * after the playerHeight latch reads the RAW value — so view.c:929's
 * (hmdPosition[1] - playerHeight) dips while ducked, but the standing
 * baseline can never get polluted by a mid-duck screen-layer flip (the
 * latch fires on every menu open/close). Controller Ys get the same
 * subtraction in webxr_input.c. */
void VR_SetHMDPosition(float x, float y, float z)
{
    float yAdj = y - WebXRLoco_GetEyeOffset();

    s_positionDelta[0] = s_worldPosition[0] - x;
    s_positionDelta[1] = s_worldPosition[1] - yAdj;
    s_positionDelta[2] = s_worldPosition[2] - z;
    s_worldPosition[0] = x;
    s_worldPosition[1] = yAdj;
    s_worldPosition[2] = z;

    hmdPosition[0] = x;
    hmdPosition[1] = yAdj;
    hmdPosition[2] = z;

    if (s_useScreenPrev != VR_UseScreenLayer())
    {
        s_useScreenPrev = VR_UseScreenLayer();
        playerHeight = y; /* RAW standing height, duck-independent */
    }
}

/* =====================================================================
 * Public queries
 * ===================================================================== */

static void WebXRBridge_OnSessionEnd(void *userData, int mode); /* fwd (self-heal below) */

/* EMSCRIPTEN_KEEPALIVE so the session-lifecycle harness
 * (test/m4-session-cycle-test.mjs) can assert the C-side flag directly —
 * a stale true here is exactly the bug-3 failure mode (blocks re-enter AND
 * suppresses the pointer-lock-exit Esc synthesis). */
EMSCRIPTEN_KEEPALIVE
bool WebXRBridge_IsSessionActive(void)
{
    /* WEBXR-PORT bug-3 self-heal: if the JS session object is gone but our
     * end callback never ran (an exception mid-teardown on the UA side —
     * see library_webxr.js PATCH #15 — or any future teardown fault), the
     * stale flag would permanently block re-entry AND suppress the
     * pointer-lock Esc fallback. Cross-check against JS truth and force the
     * end teardown ourselves. Skipped inside the XR frame pump, where the
     * session trivially exists and this query is hot. */
    static bool healing = false;
    if (s_sessionActive && !s_inXRFrame && !healing && !webxr_js_session_exists())
    {
        healing = true;
        printf("[webxr] stale session-active flag (JS session gone) — forcing end teardown\n");
        WebXRBridge_OnSessionEnd(NULL, -1);
        healing = false;
    }
    return s_sessionActive;
}

/* HMD position delta this frame (raw XR axes, meters) — the fork's
 * positionDeltaThisFrame equivalent, computed in VR_SetHMDPosition above.
 * Consumed by the M3 input foundation (webxr_input.h). */
void WebXRBridge_GetHMDPositionDelta(float out[3])
{
    out[0] = s_positionDelta[0];
    out[1] = s_positionDelta[1];
    out[2] = s_positionDelta[2];
}

/* Vertical FOV for frustum culling (cl_screen.c:2112). Derived from the
 * cached projection matrix: tanUp = (1+P[9])/P[5], tanDown = (1-P[9])/P[5].
 * XR lens frusta are asymmetric while the engine assumes symmetric, so use
 * the larger half-angle plus a small margin — slightly conservative culling,
 * never visible over-culling. */
float WebXRBridge_GetFOV(void)
{
    if (!s_sessionActive || !s_frameDataValid)
        return 0.0f;
    const float *p = s_eyeProjection[0];
    if (p[5] == 0.0f)
        return 0.0f;
    float tanUp = fabsf((1.0f + p[9]) / p[5]);
    float tanDown = fabsf((1.0f - p[9]) / p[5]);
    float tanHalf = (tanUp > tanDown ? tanUp : tanDown) * 1.05f;
    return 2.0f * atanf(tanHalf) * (180.0f / (float)M_PI);
}

/* WEBXR-PORT bug-2 (headset QA 2026-07-12): horizontal NDC shift that makes
 * a 2D overlay element drawn into eye 'eye' appear at depthMeters.
 *
 * Derivation: a world point centered between the eyes at distance D projects
 * in eye e (camera displaced laterally by ipd/2) to
 *     ndc_x = sign_e * P0 * ipd/(2D) - P8      (sign: +left eye, -right eye)
 * with P0 = proj[0] (column-major m00) and P8 = proj[8] (m02, the frustum
 * asymmetry term). A 2D element drawn at the SAME ndc in both eyes has zero
 * buffer disparity — which on a real HMD is NOT "at infinity": the per-eye
 * asymmetric (outward-canted) frusta mean equal ndc = divergent view rays,
 * unfusable text (the QA "message text doubles between eyes" report; the
 * fork's hardcoded ±10/±20/±5 con-unit offsets never compensated P8 and
 * disagreed with each other in depth). Shifting each eye's overlay by this
 * value reproduces the projection of a real point at D.
 *
 * Units: the 0.065 m IPD matches the engine's own GetStereoSeparation()
 * (vr_worldscale * 0.065); since the render camera offset AND the target
 * depth both scale by vr_worldscale, the scale cancels and physical meters
 * can be used directly against the unitless projection terms.
 *
 * EMSCRIPTEN_KEEPALIVE so the emulated harness
 * (test/m4-hud-parallax-test.mjs) can compute the expected disparity from
 * the exact projections IWER hands us. */
EMSCRIPTEN_KEEPALIVE
float WebXRBridge_2DParallaxNDC(int eye, float depthMeters)
{
    if (!s_sessionActive || !s_frameDataValid || eye < 0 || eye >= WEBXR_EYES)
        return 0.0f;
    if (depthMeters < 0.25f)
        depthMeters = 0.25f; /* clamp: closer than 25 cm is painful and the offset explodes */
    const float *p = s_eyeProjection[eye];
    float sign = (eye == 0) ? 1.0f : -1.0f;
    return sign * p[0] * 0.065f / (2.0f * depthMeters) - p[8];
}

/* =====================================================================
 * Session lifecycle + XR frame pump
 * ===================================================================== */

static void WebXRBridge_OnSessionStart(void *userData, int mode)
{
    (void)userData;
    printf("[webxr] session started (mode %d) — pausing flatscreen loop\n", mode);
    s_sessionActive = true;
    s_needResolution = true;
    s_frameDataValid = false;
    s_projParamsSet = false;
    s_badViewCountLogged = 0;
    vrMode = 1;
    WebXRLoco_OnSessionStart(); /* WEBXR-PORT M3-loco: undo the flatscreen vr_yawmode 0 boot override */

    /* Stop the flatscreen rAF pump; the XR session's rAF now owns the
     * QC_* frame protocol. (Design doc §3.) */
    emscripten_pause_main_loop();
    webxr_js_notify_state(1);
}

static void WebXRBridge_OnSessionEnd(void *userData, int mode)
{
    (void)userData;
    printf("[webxr] session ended (mode %d) — resuming flatscreen loop\n", mode);
    s_sessionActive = false;
    s_frameDataValid = false;
    s_inXRFrame = false;
    vrMode = 0;

    /* zero controller/input state so stale buttons/poses don't leak into
     * flatscreen or the next session */
    WebXRInput_Reset();

    /* back to the canvas backbuffer */
    webxr_js_bind_canvas();

    /* WEBXR-PORT bug-3 hardening: resume the flatscreen loop and tell the
     * page FIRST — the canvas-size restore below runs a full VID_Restart
     * (GL-heavy, the riskiest teardown step); if anything in it ever throws,
     * the app must already be back in a self-consistent flat state instead
     * of a paused loop with a stale page button. */
    emscripten_resume_main_loop();
    webxr_js_notify_state(0);

    /* flatscreen render size */
    int w = 0, h = 0;
    emscripten_get_canvas_element_size("#canvas", &w, &h);
    if (w > 0 && h > 0 && (w != andrw || h != andrh))
        QC_SetResolution(w, h);
}

static void WebXRBridge_OnError(void *userData, int error)
{
    (void)userData;
    printf("[webxr] error %d (%s)\n", error,
           error == WEBXR_ERR_API_UNSUPPORTED ? "WebXR API unsupported" :
           error == WEBXR_ERR_GL_INCAPABLE ? "GL context not XR-capable" :
           error == WEBXR_ERR_SESSION_UNSUPPORTED ? "session mode unsupported" : "?");
    webxr_js_notify_state(0);
}

static void WebXRBridge_OnXRFrame(void *userData, int timeMs,
                                  WebXRRigidTransform *headPose,
                                  WebXRView views[2], int viewCount)
{
    (void)userData; (void)timeMs;

    if (!s_sessionActive)
        return;

    s_lastXRFrameMs = emscripten_get_now(); /* bug-3 self-heal: session demonstrably alive */

    /* Eye count is hard-pinned to 2 for M2 (ovrMaxNumEyes is a compile-time
     * 2 throughout the engine's calling code — design doc §3). */
    if (viewCount != WEBXR_EYES)
    {
        if (!s_badViewCountLogged++)
            printf("[webxr] unsupported view count %d (want 2) — not rendering\n", viewCount);
        return;
    }

    for (int eye = 0; eye < WEBXR_EYES; eye++)
    {
        memcpy(s_eyeProjection[eye], views[eye].projectionMatrix, sizeof(s_eyeProjection[eye]));
        memcpy(s_eyeViewport[eye], views[eye].viewport, sizeof(s_eyeViewport[eye]));
    }
    s_frameDataValid = true;

    /* First XR frame: adopt the real per-eye render size (one eye's
     * viewport, NOT the double-wide layer size — design doc §3). Triggers
     * one VID_Restart_f; the renderer restart rebinds framebuffers, so the
     * layer FBO is re-bound below before drawing. */
    if (s_needResolution)
    {
        s_needResolution = false;
        int w = s_eyeViewport[0][2], h = s_eyeViewport[0][3];
        printf("[webxr] eye buffer %dx%d (layer viewports: L %d,%d R %d,%d)\n",
               w, h, s_eyeViewport[0][0], s_eyeViewport[0][1],
               s_eyeViewport[1][0], s_eyeViewport[1][1]);
        if (w > 0 && h > 0 && (w != andrw || h != andrh))
            QC_SetResolution(w, h);
    }

    /* Head pose -> engine, mirroring TBXR_GetHMDOrientation
     * (TBXR_Common.c:1887-1911): orientation through the ported
     * QuatToYawPitchRoll, position raw (§4.3). */
    float ypr[3];
    WebXRBridge_QuatToYawPitchRoll(headPose->orientation, NULL, ypr);
    VR_SetHMDPosition(headPose->position[0], headPose->position[1], headPose->position[2]);
    VR_SetHMDOrientation(ypr[0], ypr[1], ypr[2]);

    /* M3 foundation: snapshot both controllers (grip+aim poses, gamepad
     * buttons/axes) into webxr_input.c's raw + TBXR-compatible state, tick
     * haptic channels, feed the vr_inputdebug dump. Must run inside the
     * frame callback (poses need the live XRFrame) and before QC_BeginFrame
     * so gameplay code (the four M3 chunks) sees this frame's state. */
    WebXRInput_Update(emscripten_get_now());

    IN_Menu_HandleInput(); /* WEBXR-PORT M3-hud: menu toggle + big-screen d-pad nav */

    /* WEBXR-PORT M3-hud: engine in 2D-UI mode (menu/console/loading/demo) ->
     * render flat into the UI FBO and draw a world-anchored quad per eye
     * instead of the head-locked stereo render */
    if (VRMenuQuad_RunFrame(views)) { WebHost_PersistTick(); return; }

    /* WEBXR-PORT M3-weapon: gunangles are controller-driven now (in_weapon.c,
     * dispatched from WebXRInput_Update above). Head aim (M2 behavior) stays
     * as the fallback for frames with no located controller aim pose — the
     * fork sends gunangles, not viewangles, to the server (cl_input.c:1845). */
    /* WEBXR-PORT M3-comfort: bullet-time (recenter fires from in_menu.c's
     * long-press) — needs this frame's fresh state (just updated above) and
     * must run before QC_BeginFrame so its cvar writes land this frame (see
     * in_comfort.h). */
    WebXRComfort_Update();

    if (!IN_Weapon_AimActive())
    {
        gunangles[0] = hmdorientation[0];
        gunangles[1] = hmdorientation[1];
        gunangles[2] = 0.0f;
    }

    /* the same pump AppThreadFunction ran (QuakeQuest_OpenXR.c:276-310),
     * minus OpenXR swapchain calls (WebXR submit is implicit on return) */
    s_inXRFrame = true;
    QC_MoveEvent(hmdorientation[1], hmdorientation[0], hmdorientation[2]); /* yaw, pitch, roll */
    QC_BeginFrame(false);

    for (int eye = 0; eye < WEBXR_EYES; eye++)
    {
        const int *vp = s_eyeViewport[eye];

        /* the layer framebuffer may have been unbound by VID_Restart_f or
         * engine-internal FBO passes on a previous eye — re-bind raw */
        webxr_js_bind_layer_fbo();

        /* per-eye clear, scissored to this eye's sub-rect of the SHARED
         * layer framebuffer (WebXR model; the right eye's x is non-zero,
         * unlike OpenXR's per-eye FBOs — design doc §5) */
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        glEnable(GL_SCISSOR_TEST);
        glScissor(vp[0], vp[1], vp[2], vp[3]);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);

        /* x,y ride to r_refdef.view.x/y and place this eye's 3D view and
         * 2D/HUD stage inside the shared framebuffer */
        QC_DrawFrame(eye, vp[0], vp[1]);
    }

    QC_EndFrame();
    s_inXRFrame = false;

    WebHost_PersistTick(); /* config/save persistence keeps running in VR */
}

/* =====================================================================
 * Init + JS-facing session controls
 * ===================================================================== */

void WebXRBridge_Init(void)
{
    webxr_init(WebXRBridge_OnXRFrame,
               WebXRBridge_OnSessionStart,
               WebXRBridge_OnSessionEnd,
               WebXRBridge_OnError,
               NULL);
    WebXRInput_Init(); /* M3 foundation: vr_inputdebug cmd + ?inputdebug=1 */
    WebXRComfort_Init(); /* WEBXR-PORT M3-comfort: vr_recenter cmd */
    printf("[webxr] bridge initialised\n");
}

EMSCRIPTEN_KEEPALIVE
void WebXRBridge_RequestSession(void)
{
    if (s_sessionActive)
    {
        /* WEBXR-PORT bug-3 self-heal: a click on the 2D page can only happen
         * when no immersive session is really presenting (immersive-vr owns
         * the display). If the flag still says active, the previous teardown
         * chain died mid-way (bug 3): JS session object gone, or a session
         * object that stopped delivering frames. Force the end teardown and
         * continue with the fresh request instead of bricking re-entry. */
        double sinceFrame = emscripten_get_now() - s_lastXRFrameMs;
        bool jsGone = !webxr_js_session_exists();
        bool frameStale = (s_lastXRFrameMs <= 0.0) || sinceFrame > 2000.0;
        if (!jsGone && !frameStale)
            return; /* genuinely active */
        printf("[webxr] stale session state on request (jsSession=%d, lastFrame %.0f ms ago) — forcing teardown\n",
               !jsGone, sinceFrame);
        webxr_js_drop_session();
        WebXRBridge_OnSessionEnd(NULL, -1);
    }
    printf("[webxr] requesting immersive-vr session\n");
    /* local-floor required (the fork's height model builds on a floor-level
     * origin: playerHeight/vieworg[2], view.c:929); bounded-floor optional */
    webxr_request_session(WEBXR_SESSION_MODE_IMMERSIVE_VR,
                          WEBXR_SESSION_FEATURE_LOCAL_FLOOR,
                          WEBXR_SESSION_FEATURE_BOUNDED_FLOOR);
}

EMSCRIPTEN_KEEPALIVE
void WebXRBridge_RequestExit(void)
{
    if (!s_sessionActive)
        return;
    webxr_request_exit();
}
