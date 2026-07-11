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

/* gl_backend.c calls this to let a VR host override the projection matrix.
 * Returning without touching 'projection' keeps the engine's own flatscreen
 * perspective matrix. (The Android host always overwrote it from OpenXR fov.) */
bool VR_GetVRProjection(int eye, float zNear, float zFar, float *projection)
{
	(void)eye; (void)zNear; (void)zFar; (void)projection;
	return false;
}

/* true => "2D big screen" mode: zero stereo eye separation (gl_rmain.c:58)
 * and no per-eye crosshair/HUD offsets (sbar.c:141). Right for flatscreen. */
qboolean VR_UseScreenLayer(void)
{
	return true;
}

/* cl_screen.c: vertical FOV in degrees (Android host returned the HMD fov_y;
 * the fork removed the scr_fov cvar). 90 = classic Quake default. */
float GetFOV(void)
{
	return 90.0f;
}

/* host.c/sv_main.c: server tick length. Android returned 1/refresh-rate.
 * For a local single-player game the engine syncs to client frametime
 * unless sv_fixedframeratesingleplayer is set, so this is a fallback. */
float GetSysTicrate(void)
{
	return 1.0f / 60.0f;
}

/* cl_screen.c/console.c/menu.c toggle the VR "big screen" (2D menu quad)
 * through this. Flatscreen: nothing to do. */
void BigScreenMode(int mode)
{
	(void)mode;
}

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
	s_pointerlocked = e->isActive;
	return EM_TRUE;
}

/* =====================================================================
 * Frame pump — the browser-side equivalent of AppThreadFunction()'s
 * while-loop (QuakeQuest_OpenXR.c:276-310), single eye, flatscreen.
 * ===================================================================== */
static void web_frame(void)
{
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

	/* ---- engine args (mirrors sys_linux.c main) ---- */
	static const char *args[] = { "quake" };
	com_argc = 1;
	com_argv = args;
	(void)argc; (void)argv;

	/* ---- engine init (Host_Main = Host_Init, no loop) ---- */
	Host_Main();
	printf("[web-host] engine initialised\n");

	/* Flatscreen configuration + keyboard/mouse binds, queued after
	 * quake.rc/config.cfg so they win over saved VR settings. */
	Cbuf_AddText(
		"vr_yawmode 0\n"          /* IN_Move: absolute yaw from QC_MoveEvent */
		"cl_trackingmode 0\n"     /* 3DoF viewmodel path — classic gun-follows-view */
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
		"bind MWHEELUP impulse 10\n"
		"bind MWHEELDOWN impulse 12\n"
	);

	/* ---- input hooks ---- */
	emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
	emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, NULL, EM_TRUE, on_key);
	emscripten_set_mousedown_callback("#canvas", NULL, EM_TRUE, on_mousebutton);
	emscripten_set_mouseup_callback("#canvas", NULL, EM_TRUE, on_mousebutton);
	emscripten_set_mousemove_callback("#canvas", NULL, EM_TRUE, on_mousemove);
	emscripten_set_wheel_callback("#canvas", NULL, EM_TRUE, on_wheel);
	emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, NULL, EM_TRUE, on_pointerlockchange);

	/* ---- browser drives the frame pump (rAF timing) ---- */
	emscripten_set_main_loop(web_frame, 0, EM_FALSE);
	return 0; /* runtime stays alive; main loop keeps running */
}
