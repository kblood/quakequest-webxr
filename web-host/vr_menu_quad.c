/*
 * vr_menu_quad.c — world-anchored 2D-UI quad for VR (Milestone 3, HUD/menus).
 * See vr_menu_quad.h for the design summary.
 *
 * How a screen-layer frame works (vs the fork's compositor quad):
 *  1. The engine's 2D-UI state decides the mode: VR_UseScreenLayer()
 *     (main_web.c) is the fork's exact predicate — bigScreen (set by the
 *     engine's BigScreenMode(1/0) calls in menu.c/console.c/cl_screen.c),
 *     demo playback, or console-active (covers connecting/loading, which
 *     force the console via KEY_CONSOLEACTIVE_FORCED, cl_screen.c:699-702).
 *  2. On the transition INTO the mode, the current head position and yaw are
 *     latched as the anchor (the fork froze playerYaw the same way,
 *     QuakeQuest_OpenXR.c:185-193, but kept following head POSITION per
 *     frame; we deliberately freeze both — "fixed in world at menu-open
 *     time", the head-locked failure mode is what M3 is fixing).
 *  3. The whole engine frame (3D world if in-game + menu/console 2D stage)
 *     is pumped ONCE, flat (r_stereo_side 0, zero stereo separation because
 *     VR_UseScreenLayer() is true, engine's own projection because the
 *     bridge's s_inXRFrame stays false) into an offscreen FBO at the
 *     engine's current render size. The FBO is created raw in JS and
 *     registered in GL.framebuffers exactly like fbo_smoketest.c/
 *     library_webxr.js, so the engine's R_Mesh_Start() GL_FRAMEBUFFER_BINDING
 *     fetch keeps returning to it after internal FBO passes (M2-validated).
 *  4. Per eye: bind the XR layer framebuffer (or the canvas backbuffer when
 *     the layer has no framebuffer, e.g. IWER), clear the eye's sub-rect,
 *     draw the FBO texture on a quad with MVP = XRView.projection *
 *     inverse(XRView.pose) * anchor-model. The draw is attribute-less
 *     (gl_VertexID, GLSL ES 3.00) and saves/restores every piece of GL state
 *     it touches so the engine's gl_state caches stay coherent.
 *
 * Quad placement = the fork's formula (TBXR_Common.c:2146-2153) with the
 * anchor latched instead of live, and distance/width on cvars:
 *   center = anchorPos + (-sin(yaw), 0, -cos(yaw)) * vr_menu_distance
 *   orientation = rotate about +Y by anchor yaw; height from texture aspect.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <math.h>
#include <string.h>

#include <emscripten.h>
#include <GLES2/gl2.h>

#include "quakedef.h"
#include "keys.h"   /* key_consoleactive, key_dest */
#include "menu.h"   /* m_state (fork parity: freeze view input while menu up) */

#include "vr_menu_quad.h"

/* ---- engine entry points (vid_android.c) ---- */
void QC_BeginFrame(bool stopTime);
void QC_DrawFrame(int eye, int x, int y);
void QC_EndFrame(void);
void QC_MoveEvent(float yaw, float pitch, float roll);
extern int andrw, andrh;          /* current engine render size */
extern float gunangles[3];        /* view.c — M2 aims with the head */

/* ---- host globals ---- */
extern float hmdorientation[3];   /* [pitch,yaw,roll] degrees (webxr_bridge.c) */
extern float hmdPosition[3];      /* raw XR-space head position, meters */
qboolean VR_UseScreenLayer(void); /* main_web.c — the fork's 2D-UI predicate */

/* =====================================================================
 * State
 * ===================================================================== */

/* the fork's big-screen flag + setter, verbatim (QuakeQuest_OpenXR.c:95,107) */
int bigScreen = 0;

void BigScreenMode(int mode)
{
	if (bigScreen != 2)
	{
		bigScreen = mode;
	}
}

static cvar_t vr_menu_distance = {CVAR_SAVE, "vr_menu_distance", "2.0", "distance (meters) of the VR menu/console screen from where you stood when it opened"};
static cvar_t vr_menu_width    = {CVAR_SAVE, "vr_menu_width",    "2.5", "width (meters) of the VR menu/console screen (height follows the render aspect)"};

static bool  s_active = false;      /* in screen-layer mode (anchor latched) */
static float s_anchorPos[3];        /* head position at mode entry (XR meters) */
static float s_anchorYaw = 0.0f;    /* head yaw at mode entry (degrees) */
static int   s_fboW = 0, s_fboH = 0;

#define MQ_DEG2RAD(a) ((a) * (float)M_PI / 180.0f)

/* =====================================================================
 * JS side: FBO management + state-safe quad draw
 * ===================================================================== */

/* Create (or recreate on size change) the offscreen UI framebuffer.
 * Same pattern as fbo_smoketest.c: raw GL objects, registered in emscripten's
 * GL.framebuffers table with fb.name = id so the engine's
 * glGetIntegerv(GL_FRAMEBUFFER_BINDING) sees a usable id (R_Mesh_Start).
 * Prior texture/renderbuffer bindings restored. Returns 1 on success. */
EM_JS(int, menuquad_js_ensure_fbo, (int w, int h), {
	var gl = GLctx;
	var mq = Module.__menuquad;
	if (mq && mq.w === w && mq.h === h) return 1;
	if (mq) {
		gl.deleteFramebuffer(mq.fb);
		gl.deleteTexture(mq.tex);
		gl.deleteRenderbuffer(mq.rb);
		GL.framebuffers[mq.id] = null;
		Module.__menuquad = null;
	}
	var prevTex = gl.getParameter(gl.TEXTURE_BINDING_2D);
	var prevRb = gl.getParameter(gl.RENDERBUFFER_BINDING);
	var prevFb = gl.getParameter(gl.FRAMEBUFFER_BINDING);

	var fb = gl.createFramebuffer();
	var tex = gl.createTexture();
	gl.bindTexture(gl.TEXTURE_2D, tex);
	gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
	var rb = gl.createRenderbuffer();
	gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
	gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, w, h);

	gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
	gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
	gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, rb);
	var status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);

	gl.bindTexture(gl.TEXTURE_2D, prevTex);
	gl.bindRenderbuffer(gl.RENDERBUFFER, prevRb);
	gl.bindFramebuffer(gl.FRAMEBUFFER, prevFb);

	if (status !== gl.FRAMEBUFFER_COMPLETE) {
		gl.deleteFramebuffer(fb); gl.deleteTexture(tex); gl.deleteRenderbuffer(rb);
		console.warn('[menuquad] FBO incomplete: 0x' + status.toString(16));
		return 0;
	}
	var id = GL.getNewId(GL.framebuffers);
	fb.name = id;
	GL.framebuffers[id] = fb;
	Module.__menuquad = { fb: fb, tex: tex, rb: rb, w: w, h: h, id: id };
	return 1;
});

/* Bind the UI framebuffer raw (engine renders the flat frame into it). */
EM_JS(void, menuquad_js_bind_fbo, (void), {
	GLctx.bindFramebuffer(GLctx.FRAMEBUFFER, Module.__menuquad.fb);
});

/* Bind the XR layer framebuffer — or the canvas backbuffer when the layer
 * exposes none (spec-legal; IWER's emulated layer does this). Unlike the
 * bridge's helper this must NOT skip the null case: the UI FBO is currently
 * bound and the quad has to land in the layer target either way. */
EM_JS(void, menuquad_js_bind_layer, (void), {
	var s = Module['webxr_session'];
	var fb = (s && s.renderState.baseLayer) ? s.renderState.baseLayer.framebuffer : null;
	GLctx.bindFramebuffer(GLctx.FRAMEBUFFER, fb);
});

/* Draw the UI texture as a quad with the given MVP into the CURRENT viewport
 * of the CURRENTLY BOUND framebuffer. Attribute-less (gl_VertexID) so no
 * vertex-array/VBO state is touched; everything else touched is saved and
 * restored so the engine's gl_state caches stay truthful. */
EM_JS(void, menuquad_js_draw, (const float *mvp), {
	var gl = GLctx;
	var mq = Module.__menuquad;
	if (!mq) return;

	if (!Module.__menuquadProg) {
		var vs = gl.createShader(gl.VERTEX_SHADER);
		gl.shaderSource(vs,
			'#version 300 es\n' +
			'uniform mat4 mvp;\n' +
			'out vec2 uv;\n' +
			'void main() {\n' +
			'  vec2 p = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n' +
			'  uv = p * 0.5 + 0.5;\n' +
			'  gl_Position = mvp * vec4(p, 0.0, 1.0);\n' +
			'}\n');
		gl.compileShader(vs);
		var fs = gl.createShader(gl.FRAGMENT_SHADER);
		gl.shaderSource(fs,
			'#version 300 es\n' +
			'precision mediump float;\n' +
			'uniform sampler2D tex;\n' +
			'in vec2 uv;\n' +
			'out vec4 fragColor;\n' +
			'void main() { fragColor = vec4(texture(tex, uv).rgb, 1.0); }\n');
		gl.compileShader(fs);
		var prog = gl.createProgram();
		gl.attachShader(prog, vs);
		gl.attachShader(prog, fs);
		gl.linkProgram(prog);
		if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) {
			console.error('[menuquad] program link failed: ' + gl.getProgramInfoLog(prog));
			return;
		}
		Module.__menuquadProg = prog;
		Module.__menuquadMvpLoc = gl.getUniformLocation(prog, 'mvp');
		Module.__menuquadTexLoc = gl.getUniformLocation(prog, 'tex');
	}

	/* ---- save the state we are about to touch ---- */
	var prevProg = gl.getParameter(gl.CURRENT_PROGRAM);
	var prevActive = gl.getParameter(gl.ACTIVE_TEXTURE);
	gl.activeTexture(gl.TEXTURE0);
	var prevTex0 = gl.getParameter(gl.TEXTURE_BINDING_2D);
	var prevDepth = gl.isEnabled(gl.DEPTH_TEST);
	var prevBlend = gl.isEnabled(gl.BLEND);
	var prevCull = gl.isEnabled(gl.CULL_FACE);
	var prevScissor = gl.isEnabled(gl.SCISSOR_TEST);
	var prevStencil = gl.isEnabled(gl.STENCIL_TEST);
	var prevDepthMask = gl.getParameter(gl.DEPTH_WRITEMASK);
	var prevColorMask = gl.getParameter(gl.COLOR_WRITEMASK);
	/* attribute-less draw still needs every ENABLED array to be legally
	 * sourced for the vertex count, so park enabled arrays and restore */
	var maxAttribs = gl.getParameter(gl.MAX_VERTEX_ATTRIBS);
	var enabledAttribs = [];
	for (var i = 0; i < maxAttribs; ++i) {
		if (gl.getVertexAttrib(i, gl.VERTEX_ATTRIB_ARRAY_ENABLED)) {
			enabledAttribs.push(i);
			gl.disableVertexAttribArray(i);
		}
	}

	/* ---- draw ---- */
	gl.useProgram(Module.__menuquadProg);
	gl.bindTexture(gl.TEXTURE_2D, mq.tex);
	gl.uniform1i(Module.__menuquadTexLoc, 0);
	gl.uniformMatrix4fv(Module.__menuquadMvpLoc, false, HEAPF32, mvp >> 2, 16);
	gl.disable(gl.DEPTH_TEST);
	gl.disable(gl.BLEND);
	gl.disable(gl.CULL_FACE);
	gl.disable(gl.SCISSOR_TEST);
	gl.disable(gl.STENCIL_TEST);
	gl.depthMask(false);
	gl.colorMask(true, true, true, true);
	gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

	/* ---- restore ---- */
	for (var j = 0; j < enabledAttribs.length; ++j)
		gl.enableVertexAttribArray(enabledAttribs[j]);
	gl.depthMask(prevDepthMask);
	gl.colorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
	if (prevDepth) gl.enable(gl.DEPTH_TEST);
	if (prevBlend) gl.enable(gl.BLEND);
	if (prevCull) gl.enable(gl.CULL_FACE);
	if (prevScissor) gl.enable(gl.SCISSOR_TEST);
	if (prevStencil) gl.enable(gl.STENCIL_TEST);
	gl.bindTexture(gl.TEXTURE_2D, prevTex0);
	gl.activeTexture(prevActive);
	gl.useProgram(prevProg);
});

/* =====================================================================
 * Column-major 4x4 helpers (GL layout: m[col*4 + row])
 * ===================================================================== */

static void MQ_MatMul(float out[16], const float a[16], const float b[16])
{
	int c, r, k;
	float t[16];
	for (c = 0; c < 4; c++)
		for (r = 0; r < 4; r++)
		{
			float s = 0.0f;
			for (k = 0; k < 4; k++)
				s += a[k * 4 + r] * b[c * 4 + k];
			t[c * 4 + r] = s;
		}
	memcpy(out, t, sizeof(t));
}

/* Inverse of a rigid transform (rotation + translation): R^T, -R^T t. */
static void MQ_MatRigidInverse(float out[16], const float m[16])
{
	int c, r;
	for (c = 0; c < 3; c++)
		for (r = 0; r < 3; r++)
			out[c * 4 + r] = m[r * 4 + c]; /* transpose rotation */
	out[3]  = 0.0f; out[7] = 0.0f; out[11] = 0.0f; out[15] = 1.0f;
	for (r = 0; r < 3; r++)
		out[12 + r] = -(out[0 + r] * m[12] + out[4 + r] * m[13] + out[8 + r] * m[14]);
}

/* Model matrix: unit quad [-1,1]^2 (facing +Z) -> scaled halfW/halfH,
 * rotated about +Y by yawDeg, translated to center. */
static void MQ_MatModel(float out[16], const float center[3], float yawDeg,
                        float halfW, float halfH)
{
	float rad = MQ_DEG2RAD(yawDeg);
	float c = cosf(rad), s = sinf(rad);
	memset(out, 0, 16 * sizeof(float));
	out[0] = c * halfW;  out[2] = -s * halfW;   /* col 0: local +X */
	out[5] = halfH;                             /* col 1: local +Y */
	out[8] = s;          out[10] = c;           /* col 2: local +Z */
	out[12] = center[0]; out[13] = center[1]; out[14] = center[2]; out[15] = 1.0f;
}

/* =====================================================================
 * Frame path
 * ===================================================================== */

void VRMenuQuad_Init(void)
{
	Cvar_RegisterVariable(&vr_menu_distance);
	Cvar_RegisterVariable(&vr_menu_width);
	Con_Printf("[menuquad] VR menu quad initialised (vr_menu_distance %s, vr_menu_width %s)\n",
	           vr_menu_distance.string, vr_menu_width.string);
}

bool VRMenuQuad_RunFrame(const WebXRView views[2])
{
	int eye;

	if (!VR_UseScreenLayer())
	{
		s_active = false;
		return false;
	}

	/* re-anchor on each entry into 2D-UI mode, never per-frame */
	if (!s_active)
	{
		s_active = true;
		s_anchorPos[0] = hmdPosition[0];
		s_anchorPos[1] = hmdPosition[1];
		s_anchorPos[2] = hmdPosition[2];
		s_anchorYaw = hmdorientation[1];
	}

	if (!menuquad_js_ensure_fbo(andrw, andrh))
		return false; /* graceful fallback: head-locked stereo render (M2) */
	s_fboW = andrw;
	s_fboH = andrh;

	/* ---- 1) flat engine frame into the UI FBO ---- */
	menuquad_js_bind_fbo();
	glViewport(0, 0, s_fboW, s_fboH);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	/* fork parity (QuakeQuest_OpenXR.c:280-284): while a menu is up, don't
	 * feed head orientation into the view */
	if (m_state == m_none)
		QC_MoveEvent(hmdorientation[1], hmdorientation[0], hmdorientation[2]);
	else
		QC_MoveEvent(0, 0, 0);

	/* keep the M2 head-aim behavior on the flat path too (chunk 2 owns the
	 * controller-driven gunangles) */
	gunangles[0] = hmdorientation[0];
	gunangles[1] = hmdorientation[1];
	gunangles[2] = 0.0f;

	QC_BeginFrame(false);
	QC_DrawFrame(0, 0, 0); /* single flat view; r_stereo_side = 0 */
	QC_EndFrame();

	/* ---- 2) world-anchored quad into each eye ---- */
	{
		float dist = vr_menu_distance.value;
		float halfW = vr_menu_width.value * 0.5f;
		float halfH = halfW * ((float)s_fboH / (float)s_fboW);
		float yawRad = MQ_DEG2RAD(s_anchorYaw);
		float center[3];
		float model[16];

		if (dist < 0.5f) dist = 0.5f;
		if (halfW < 0.25f) halfW = 0.25f;
		/* sanity cap: very tall eye buffers (e.g. emulated 400x600) would
		 * otherwise make a wall-sized portrait screen — shrink BOTH dims
		 * (aspect preserved) so the screen never exceeds 3m tall */
		if (halfH > 1.5f)
		{
			halfW *= 1.5f / halfH;
			halfH = 1.5f;
		}

		/* the fork's placement formula (TBXR_Common.c:2146-2153), anchored */
		center[0] = s_anchorPos[0] - sinf(yawRad) * dist;
		center[1] = s_anchorPos[1];
		center[2] = s_anchorPos[2] - cosf(yawRad) * dist;
		/* keep the screen's bottom edge above the floor */
		if (center[1] < halfH + 0.2f)
			center[1] = halfH + 0.2f;

		MQ_MatModel(model, center, s_anchorYaw, halfW, halfH);

		for (eye = 0; eye < 2; eye++)
		{
			const int *vp = views[eye].viewport;
			float view[16], mv[16], mvp[16];

			menuquad_js_bind_layer();

			glViewport(vp[0], vp[1], vp[2], vp[3]);
			glEnable(GL_SCISSOR_TEST);
			glScissor(vp[0], vp[1], vp[2], vp[3]);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			glDisable(GL_SCISSOR_TEST);

			/* XRView.transform is the eye pose (view->reference space);
			 * the view matrix is its rigid inverse */
			MQ_MatRigidInverse(view, views[eye].viewPose.matrix);
			MQ_MatMul(mv, view, model);
			MQ_MatMul(mvp, views[eye].projectionMatrix, mv);

			menuquad_js_draw(mvp);
		}
	}

	return true;
}

EMSCRIPTEN_KEEPALIVE
int VRMenuQuad_DebugState(void)
{
	return (s_active ? 1 : 0)
	     | ((bigScreen & 0xf) << 4)
	     | ((key_consoleactive ? 1 : 0) << 8)
	     | ((cls.demoplayback ? 1 : 0) << 9)
	     | (((int)key_dest & 0xf) << 12)
	     | (((int)m_state & 0xff) << 16);
}
