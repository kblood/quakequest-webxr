/*
 * fbo_smoketest.c — M2 risk #1 probe (reports/05-webxr-bridge-design.md §5/§8).
 *
 * Verifies that an EXTERNALLY-bound framebuffer — created in raw JS and bound
 * via GLctx.bindFramebuffer, exactly the way the vendored library_webxr.js
 * binds XRWebGLLayer.framebuffer before calling into native code — survives a
 * full QC_BeginFrame/QC_DrawFrame/QC_EndFrame under -sFULL_ES2 emulation,
 * i.e. the engine's gl_state.framebufferobject dedup cache never rebinds the
 * canvas backbuffer mid-frame and the world lands in the external FBO.
 *
 * The JS side also registers the framebuffer in Emscripten's GL.framebuffers
 * id table with fb.name = id (library_webxr.js:139-147 pattern) so that the
 * engine's R_Mesh_Start() glGetIntegerv(GL_FRAMEBUFFER_BINDING) fetch returns
 * a usable id for gl_state.defaultframebufferobject.
 *
 * Trigger from JS: Module._WebFBOTest_Request() (index.html wires this to a
 * ?fbotest=1 query param). Result: console line "[fbotest] PASS/FAIL {...}"
 * + window.__fboTestResult for probing.
 *
 * Part of the QuakeQuest->WebXR port. GPL-2.0 (see ../darkplaces/COPYING).
 */

#include <stdio.h>
#include <stdbool.h>
#include <emscripten.h>

void QC_BeginFrame(bool stopTime);
void QC_DrawFrame(int eye, int x, int y);
void QC_EndFrame(void);
extern int andrw, andrh; /* vid_android.c — current engine render size */

/* Create an offscreen FBO (RGBA8 + DEPTH24_STENCIL8), register it in
 * Emscripten's GL.framebuffers table like library_webxr.js does, and leave it
 * RAW-bound (GLctx.bindFramebuffer, bypassing the engine's qgl wrapper and
 * gl_state cache). Texture/renderbuffer bindings are restored so the engine's
 * own binding caches stay coherent. Returns the registered id, 0 on failure. */
EM_JS(int, fbotest_js_setup, (int w, int h), {
    var gl = GLctx;
    var prevTex = gl.getParameter(gl.TEXTURE_BINDING_2D);
    var prevRb = gl.getParameter(gl.RENDERBUFFER_BINDING);

    var fb = gl.createFramebuffer();
    var tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    var rb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
    gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, w, h);

    gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, rb);
    var status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);

    gl.bindTexture(gl.TEXTURE_2D, prevTex);
    gl.bindRenderbuffer(gl.RENDERBUFFER, prevRb);

    /* register in the emscripten id table exactly like library_webxr.js:143-146 */
    var id = GL.getNewId(GL.framebuffers);
    fb.name = id;
    GL.framebuffers[id] = fb;
    Module.__fbotest = { fb: fb, tex: tex, rb: rb, w: w, h: h, id: id };

    /* clear to a known sentinel so "engine drew nothing" is detectable */
    gl.clearColor(0, 0, 0, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
    /* leave fb RAW-bound — the engine frame must keep drawing into it */
    return (status === gl.FRAMEBUFFER_COMPLETE) ? id : 0;
});

/* Read back from the external FBO after the engine frame: count non-black
 * pixels and coarse color diversity across three scanlines. A rendered game
 * world gives high diversity; an untouched FBO stays sentinel-black. */
EM_JS(int, fbotest_js_analyze, (), {
    var gl = GLctx;
    var t = Module.__fbotest;
    gl.bindFramebuffer(gl.FRAMEBUFFER, t.fb);
    var w = t.w, h = t.h;
    var nonblack = 0, total = 0;
    var colors = new Set();
    var px = new Uint8Array(w * 4);
    [0.25, 0.5, 0.75].forEach(function(r) {
        gl.readPixels(0, (h * r) | 0, w, 1, gl.RGBA, gl.UNSIGNED_BYTE, px);
        for (var i = 0; i < w; i++) {
            var R = px[i*4], G = px[i*4+1], B = px[i*4+2];
            total++;
            if (R + G + B > 24) nonblack++;
            colors.add((R >> 4) + ',' + (G >> 4) + ',' + (B >> 4));
        }
    });
    /* cleanup: unbind (back to canvas backbuffer), unregister, delete */
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    GL.framebuffers[t.id] = null;
    gl.deleteFramebuffer(t.fb);
    gl.deleteTexture(t.tex);
    gl.deleteRenderbuffer(t.rb);
    Module.__fbotest = null;

    var frac = nonblack / total;
    var pass = (frac > 0.10) && (colors.size >= 8);
    var res = { pass: pass, nonblackFrac: Math.round(frac * 1000) / 1000,
                colorBuckets: colors.size };
    console.log('[fbotest] ' + (pass ? 'PASS' : 'FAIL') + ' ' + JSON.stringify(res));
    if (typeof window !== 'undefined') window.__fboTestResult = res;
    return pass ? 1 : 0;
});

static int s_requested = 0;

EMSCRIPTEN_KEEPALIVE
void WebFBOTest_Request(void)
{
    s_requested = 1;
}

/* Called at the top of web_frame; when armed, consumes one frame: renders the
 * engine frame into the raw-bound external FBO and reads it back. */
bool WebFBOTest_RunIfRequested(void)
{
    if (!s_requested)
        return false;
    s_requested = 0;

    printf("[fbotest] running: external FBO %dx%d, one full engine frame\n", andrw, andrh);
    if (!fbotest_js_setup(andrw, andrh))
    {
        printf("[fbotest] FAIL: framebuffer incomplete\n");
        return true;
    }

    QC_BeginFrame(false);
    QC_DrawFrame(0, 0, 0);
    QC_EndFrame();

    fbotest_js_analyze();
    return true;
}
