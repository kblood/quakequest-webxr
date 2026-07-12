/*
 * main_web.c — browser host for the QuakeQuest darkplaces fork (Milestone 1:
 * flatscreen in a normal browser tab).
 *
 * Replaces the Android/OpenXR host (QuakeQuestSrc/QuakeQuest_OpenXR.c +
 * TBXR_Common.c). The engine's main loop is already inverted — the host pumps
 * QC_BeginFrame / QC_DrawFrame(eye,x,y) / QC_EndFrame each frame, exactly as
 * AppThreadFunction() did on Android (QuakeQuest_OpenXR.c:276-310). Here the
 * browser's requestAnimationFrame drives the pump via
 * emscripten_set_main_loop.
 *
 * Also provides the VR-host-side globals/functions the engine links against
 * (VR_GetVRProjection, VR_UseScreenLayer, hmdorientation, ...) as flatscreen
 * stubs. See src/PORT_NOTES.md for the full list.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <emscripten.h>
#include <emscripten/html5.h>

#include "quakedef.h"
#include "keys.h"

#include "webxr_bridge.h"   /* M2: WebXR session/rendering bridge */
#include "vr_menu_quad.h"   /* WEBXR-PORT M3-hud: bigScreen state + menu quad */
#include "in_weapon.h"      /* WEBXR-PORT M4: cl_trackingmode preference keeper */

/* ---- engine entry points / externs (darkplaces side) ---- */
void Host_Main(void);                       /* host.c  — Host_Init() only (loop already inverted) */
void QC_BeginFrame(bool stopTime);          /* vid_android.c */
void QC_DrawFrame(int eye, int x, int y);   /* vid_android.c */
void QC_EndFrame(void);                     /* vid_android.c */
void QC_KeyEvent(int state, int key, int character); /* vid_android.c -> Key_Event */
void QC_MoveEvent(float yaw, float pitch, float roll); /* vid_android.c -> IN_Move */
extern int andrw, andrh;                    /* vid_android.c — framebuffer size used by VID_InitMode */
extern float gunangles[3];                  /* view.c — weapon aim angles; sent to server as cl.cmd.viewangles */

/* =====================================================================
 * VR host stubs — symbols the engine expects the (formerly OpenXR) host
 * layer to define. Flatscreen M1 values; M2/M3 replace these with real
 * WebXR data.
 * ===================================================================== */
extern int vrMode;               /* defined in menu.c (default 2); 0 = flat HUD in sbar.c */
float hmdorientation[3] = {0, 0, 0}; /* menu.c/sbar.c: head orientation (unused flat) */
float hmdPosition[3]    = {0, 0, 0}; /* view.c: head position (unused flat) */
float weaponOffset[3]   = {0, 0, 0}; /* view.c: 6DoF hand-vs-head offset (unused flat) */
float playerHeight      = 0.0f;      /* view.c: standing-height reference */

/* VR_GetVRProjection / VR_SetHMDOrientation / VR_SetHMDPosition are owned by
 * webxr_bridge.c since M2 (single source of truth, design doc §6/risk #4):
 * no-ops while flat, real XR data during a session. */

/* true => "2D big screen" mode: zero stereo eye separation (gl_rmain.c:58)
 * and no per-eye crosshair/HUD offsets (sbar.c:141). Right for flatscreen;
 * during an immersive session it is the fork's exact 2D-UI predicate
 * (QuakeQuest_OpenXR.c:97-100) and drives the world-anchored menu quad
 * (WEBXR-PORT M3-hud, vr_menu_quad.c). */
qboolean VR_UseScreenLayer(void)
{
	if (!WebXRBridge_IsSessionActive())
		return true;
	return (bigScreen != 0 || cls.demoplayback || key_consoleactive); /* WEBXR-PORT M3-hud */
}

/* cl_screen.c: vertical FOV in degrees (Android host returned the HMD fov_y;
 * the fork removed the scr_fov cvar). 90 = classic Quake default; in VR the
 * bridge derives it from the XR projection matrix (frustum culling only —
 * the projection itself comes from VR_GetVRProjection). */
float GetFOV(void)
{
	float xrFov = WebXRBridge_GetFOV();
	return xrFov > 0.0f ? xrFov : 90.0f;
}

/* host.c/sv_main.c: server tick length. Android returned 1/refresh-rate.
 * For a local single-player game the engine syncs to client frametime
 * unless sv_fixedframeratesingleplayer is set, so this is a fallback. */
float GetSysTicrate(void)
{
	return 1.0f / 60.0f;
}

/* cl_screen.c/console.c/menu.c toggle the VR "big screen" (2D menu quad)
 * through BigScreenMode() — since M3-hud it lives in vr_menu_quad.c and
 * tracks the engine's 2D-UI state for the world-anchored menu quad. */

/* sys_shared.c calls this from Sys_Quit. */
void QC_exit(int exitCode)
{
	printf("[web-host] engine requested exit (%d)\n", exitCode);
	emscripten_cancel_main_loop();
}

/* =====================================================================
 * Input state
 * ===================================================================== */
static float s_yaw = 0.0f;    /* accumulated absolute view yaw   (degrees) */
static float s_pitch = 0.0f;  /* accumulated absolute view pitch (degrees, + is down) */
static float s_sensitivity = 0.12f; /* degrees per pointer-locked pixel */
static bool  s_pointerlocked = false;

/* Map a browser keydown/keyup to (K_* key, ascii char). Returns 0 if unmapped. */
static int MapKey(const EmscriptenKeyboardEvent *e, int *ascii)
{
	unsigned long kc = e->keyCode;
	*ascii = 0;

	/* printable single-character keys: e->key is the produced character */
	if (e->key[0] != '\0' && e->key[1] == '\0')
	{
		unsigned char c = (unsigned char)e->key[0];
		if (c >= 32 && c < 127)
		{
			*ascii = c;
			/* engine key codes are the unshifted lowercase character */
			if (kc >= 'A' && kc <= 'Z')
				return 'a' + (int)(kc - 'A');
			if (kc >= '0' && kc <= '9')
				return (int)kc;
			return tolower(c);
		}
	}

	switch (kc)
	{
	case 8:   *ascii = 0; return K_BACKSPACE;
	case 9:   return K_TAB;
	case 13:  *ascii = 13; return K_ENTER;
	case 16:  return K_SHIFT;
	case 17:  return K_CTRL;
	case 18:  return K_ALT;
	case 19:  return K_PAUSE;
	case 20:  return K_CAPSLOCK;
	case 27:  return K_ESCAPE;
	case 32:  *ascii = 32; return K_SPACE;
	case 33:  return K_PGUP;
	case 34:  return K_PGDN;
	case 35:  return K_END;
	case 36:  return K_HOME;
	case 37:  return K_LEFTARROW;
	case 38:  return K_UPARROW;
	case 39:  return K_RIGHTARROW;
	case 40:  return K_DOWNARROW;
	case 45:  return K_INS;
	case 46:  return K_DEL;
	case 192: *ascii = '`'; return '`'; /* console toggle */
	default:
		if (kc >= 112 && kc <= 123) /* F1..F12 */
			return K_F1 + (int)(kc - 112);
		return 0;
	}
}

static EM_BOOL on_key(int type, const EmscriptenKeyboardEvent *e, void *ud)
{
	(void)ud;
	int ascii = 0;
	int key = MapKey(e, &ascii);
	if (!key)
		return EM_FALSE;
	/* let the browser keep F11 (fullscreen) and F12 (devtools) */
	if (e->keyCode == 122 || e->keyCode == 123)
		return EM_FALSE;
	if (e->repeat && key != K_BACKSPACE && !(key >= 32 && key < 127))
		return EM_TRUE;
	QC_KeyEvent(type == EMSCRIPTEN_EVENT_KEYDOWN ? 1 : 0, key, ascii);
	return EM_TRUE; /* preventDefault: keep space/arrows from scrolling the page */
}

static EM_BOOL on_mousebutton(int type, const EmscriptenMouseEvent *e, void *ud)
{
	(void)ud;
	int down = (type == EMSCRIPTEN_EVENT_MOUSEDOWN);
	int key;
	switch (e->button)
	{
	case 0: key = K_MOUSE1; break;
	case 1: key = K_MOUSE3; break;
	case 2: key = K_MOUSE2; break;
	default: key = 0; break;
	}

	if (down && !s_pointerlocked)
	{
		/* first click captures the mouse instead of firing */
		emscripten_request_pointerlock("#canvas", EM_FALSE);
		return EM_TRUE;
	}
	if (key)
		QC_KeyEvent(down, key, 0);
	return EM_TRUE;
}

static EM_BOOL on_mousemove(int type, const EmscriptenMouseEvent *e, void *ud)
{
	(void)type; (void)ud;
	if (!s_pointerlocked)
		return EM_FALSE;
	s_yaw   -= (float)e->movementX * s_sensitivity;
	s_pitch += (float)e->movementY * s_sensitivity;
	if (s_pitch >  89.0f) s_pitch =  89.0f;
	if (s_pitch < -89.0f) s_pitch = -89.0f;
	while (s_yaw >  180.0f) s_yaw -= 360.0f;
	while (s_yaw < -180.0f) s_yaw += 360.0f;
	return EM_TRUE;
}

static EM_BOOL on_wheel(int type, const EmscriptenWheelEvent *e, void *ud)
{
	(void)type; (void)ud;
	int key = (e->deltaY < 0) ? K_MWHEELUP : K_MWHEELDOWN;
	QC_KeyEvent(1, key, 0);
	QC_KeyEvent(0, key, 0);
	return EM_TRUE;
}

static EM_BOOL on_pointerlockchange(int type, const EmscriptenPointerlockChangeEvent *e, void *ud)
{
	(void)type; (void)ud;
	bool was = s_pointerlocked;
	s_pointerlocked = e->isActive;

	/* WEBXR-PORT: while pointer-locked the browser reserves Esc for the
	 * unlock itself — the keydown never reaches the page, so in-game Esc
	 * appeared to "only release the mouse". Treat losing the lock during
	 * gameplay as the Esc it was: open the menu (pause), matching native
	 * Quake. Guarded to gameplay only so menu/console navigation and XR
	 * sessions (no pointer lock) are unaffected. */
	if (was && !e->isActive && cls.state != ca_disconnected
	    && key_dest == key_game && !WebXRBridge_IsSessionActive())
	{
		QC_KeyEvent(1, K_ESCAPE, 0);
		QC_KeyEvent(0, K_ESCAPE, 0);
	}
	return EM_TRUE;
}

/* =====================================================================
 * Frame pump — the browser-side equivalent of AppThreadFunction()'s
 * while-loop (QuakeQuest_OpenXR.c:276-310), single eye, flatscreen.
 * ===================================================================== */
bool WebFBOTest_RunIfRequested(void); /* fbo_smoketest.c — M2 risk-#1 probe */

/* =====================================================================
 * Config/save persistence (M1 issue #4): /quake_user is an IDBFS mount
 * (set up in index.html preRun, populated with FS.syncfs(true) before
 * main runs); the engine writes there via -userdir. Persist = write
 * config.cfg + push the mount to IndexedDB.
 * ===================================================================== */
void Host_SaveConfig(void); /* host.c — writes key binds + archived cvars */

EM_JS(void, web_js_idbfs_sync, (void), {
	if (Module.__idbfsSyncing) return;      /* coalesce overlapping syncs */
	Module.__idbfsSyncing = true;
	FS.syncfs(false, function(err) {
		Module.__idbfsSyncing = false;
		if (err) console.warn('[persist] FS.syncfs failed:', err);
	});
});

static double s_lastPersistMs = 0.0;

/* Exported for the page's visibilitychange/pagehide handler. */
EMSCRIPTEN_KEEPALIVE
void WebHost_PersistNow(void)
{
	/* WEBXR-PORT M4 (reports/08b issue 3): cl_trackingmode is CVAR_SAVE but
	 * the mode-switch code forces it per-context (0 flatscreen, pref in VR);
	 * this save variant writes the USER PREFERENCE to config.cfg, not the
	 * currently forced runtime value. */
	IN_Weapon_SaveConfigPreservingTrackingMode();
	web_js_idbfs_sync();
	s_lastPersistMs = emscripten_get_now();
}

/* WEBXR-PORT M4: full-game data drop-in. The page writes user-supplied
 * pak files (e.g. registered pak1.pak) into /quake_user/id1/ (the IDBFS
 * mount -userdir points at) and calls this so the running engine picks
 * them up: fs_rescan rebuilds the search path (FS_AddGameDirectory scans
 * userdir/id1/*.pak, userdir wins over the preloaded shareware pak) and
 * re-checks gfx/pop.lmp to flip the `registered` cvar. Game data stays
 * browser-local — nothing is ever sent to a server. */
EMSCRIPTEN_KEEPALIVE
void WebHost_RescanFS(void)
{
	Cbuf_AddText("fs_rescan\n");
}

/* Called once per pumped frame (flatscreen loop AND XR loop). */
void WebHost_PersistTick(void)
{
	double now = emscripten_get_now();
	if (now - s_lastPersistMs > 10000.0)
		WebHost_PersistNow();
}

static void web_frame(void)
{
	WebHost_PersistTick();

	/* WEBXR-PORT M4: pick up user changes to cl_trackingmode (console/menu)
	 * on flatscreen frames too — XR frames tick inside IN_Weapon_Update. */
	IN_Weapon_TrackingModeTick();

	/* M2 FBO smoke test: when armed, this frame renders into an external,
	 * raw-bound FBO instead of the canvas (see fbo_smoketest.c) */
	if (WebFBOTest_RunIfRequested())
		return;

	/* absolute mouse-look angles; vr_yawmode 0 applies them verbatim in IN_Move */
	QC_MoveEvent(s_yaw, s_pitch, 0.0f);

	/* flatscreen: gun aims where the view looks. gunangles is what
	 * CL_UpdateMove copies into cl.cmd.viewangles (cl_input.c:1845). */
	gunangles[0] = s_pitch;
	gunangles[1] = s_yaw;
	gunangles[2] = 0.0f;

	QC_BeginFrame(false);
	QC_DrawFrame(0, 0, 0);  /* eye 0 only */
	QC_EndFrame();
}

int main(int argc, char **argv)
{
	printf("[web-host] QuakeQuest web host starting\n");
	vrMode = 0; /* flatscreen */

	/* ---- WebGL2 context on #canvas, made current before any engine GL ---- */
	EmscriptenWebGLContextAttributes attr;
	emscripten_webgl_init_context_attributes(&attr);
	attr.majorVersion = 2;  /* WebGL2 — accepts the engine's GLSL ES 1.00 shaders */
	attr.minorVersion = 0;
	attr.alpha = EM_FALSE;
	attr.depth = EM_TRUE;
	attr.stencil = EM_TRUE;
	attr.antialias = EM_FALSE;
	attr.preserveDrawingBuffer = EM_FALSE;
	attr.enableExtensionsByDefault = EM_TRUE;
	attr.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;

	EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attr);
	if (ctx <= 0)
	{
		printf("[web-host] FATAL: WebGL2 context creation failed (%d)\n", (int)ctx);
		return 1;
	}
	emscripten_webgl_make_context_current(ctx);

	/* ---- resolution: use the canvas backing-store size ---- */
	int w = 0, h = 0;
	emscripten_get_canvas_element_size("#canvas", &w, &h);
	if (w <= 0 || h <= 0) { w = 1280; h = 720; }
	andrw = w;
	andrh = h;
	printf("[web-host] canvas %dx%d\n", w, h);

	/* ---- game data: preloaded by emcc --preload-file at /quake/id1 ---- */
	if (chdir("/quake") != 0)
		printf("[web-host] WARNING: chdir /quake failed — game data missing?\n");

	/* ---- engine args (mirrors sys_linux.c main) ----
	 * -userdir /quake_user: writable IDBFS mount for config.cfg/saves
	 * (mounted+preloaded by index.html preRun; M1 issue #4).
	 * WEBXR-PORT: any argv from Module.callMain([...]) is appended so the
	 * page (or a headless test harness) can hand the engine command-line
	 * options like "-benchmark demo1" or "+timedemo demo1". argv[0] is
	 * always replaced with "quake". */
	static const char *args[64] = { "quake", "-userdir", "/quake_user" };
	com_argc = 3;
	for (int ai = 1; ai < argc && com_argc < (int)(sizeof(args)/sizeof(args[0])); ai++)
		args[com_argc++] = argv[ai];
	com_argv = args;

	/* ---- engine init (Host_Main = Host_Init, no loop) ---- */
	Host_Main();
	printf("[web-host] engine initialised\n");

	/* WEBXR-PORT / M2: WebGL has no client-side vertex/index array path at
	 * all, unlike desktop GL. M1 worked around this by disabling VBOs
	 * entirely (gl_vbo 0) and letting -sFULL_ES2 emulate every draw call
	 * with a scratch-buffer copy. M2 instead forces the engine's own
	 * forcevbo mechanism on (gl_webgl_forcevbo 1, default-on, see
	 * gl_backend.c R_Mesh_SetUseVBO) so every draw is genuinely VBO-sourced
	 * — gl_vbo/gl_vbo_dynamicvertex/gl_vbo_dynamicindex are left at their
	 * normal defaults and no longer need to be forced from here at all. */

	/* Flatscreen configuration + keyboard/mouse binds, queued after
	 * quake.rc/config.cfg so they win over saved VR settings. */
	/* cl_trackingmode is NOT forced here anymore (M4, reports/08b issue 3):
	 * it is CVAR_SAVE, so stomping it from the command buffer clobbered the
	 * user's saved preference. in_weapon.c now owns it — IN_Weapon_Init
	 * latches the config.cfg value as the preference, then forces the
	 * flatscreen 3DoF value 0 at runtime only. */
	Cbuf_AddText(
		"vr_yawmode 0\n"          /* IN_Move: absolute yaw from QC_MoveEvent */
		"bind w +forward\n"
		"bind s +back\n"
		"bind a +moveleft\n"
		"bind d +moveright\n"
		"bind SPACE +jump\n"
		"bind e +jump\n"
		"bind c +movedown\n"
		"bind SHIFT +speed\n"
		"bind CTRL +attack\n"
		"bind MOUSE1 +attack\n"
		"bind MOUSE2 +jump\n"
		"bind MWHEELUP \"impulse 10\"\n"
		"bind MWHEELDOWN \"impulse 12\"\n"
	);

	/* ---- input hooks ---- */
	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
	emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
	emscripten_set_mousedown_callback("#canvas", NULL, EM_TRUE, on_mousebutton);
	emscripten_set_mouseup_callback("#canvas", NULL, EM_TRUE, on_mousebutton);
	emscripten_set_mousemove_callback("#canvas", NULL, EM_TRUE, on_mousemove);
	emscripten_set_wheel_callback("#canvas", NULL, EM_TRUE, on_wheel);
	emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, on_pointerlockchange);

	/* ---- WebXR bridge (M2): registers session callbacks; the "Enter VR"
	 * button calls _WebXRBridge_RequestSession from its click handler ---- */
	WebXRBridge_Init();
	VRMenuQuad_Init(); /* WEBXR-PORT M3-hud: vr_menu_distance/vr_menu_width cvars */

	/* ---- browser drives the frame pump (rAF timing) ---- */
	emscripten_set_main_loop(web_frame, 0, EM_FALSE);
	return 0; /* runtime stays alive; main loop keeps running */
}
