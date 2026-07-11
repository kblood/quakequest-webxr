# PORT_NOTES — M1 (flatscreen) + M2 (WebXR stereo)

State as of 2026-07-12: **M1 + M2 complete (pending real-headset confirm).**
Engine links, boots shareware Quake flatscreen at 1280x720; entering VR starts
an immersive-vr session, pauses the flatscreen loop, and renders head-tracked
per-eye stereo into the XRWebGLLayer framebuffer. Config/saves persist via
IDBFS. Verified headless incl. a full emulated XR session (IWER): enter →
stereo render → exit → re-enter, zero console/page errors.

## M2 architecture (see reports/05-webxr-bridge-design.md — implemented as designed)

- `web-host/lib/webxr/` — vendored emscripten-webxr @1bc0b7b with **11
  patches**, all marked `WEBXR-PORT PATCH #n` and documented in
  `web-host/lib/webxr/PATCHES.md` (the 3 design-doc bugs + 8 more found
  during bring-up, incl. head-pose position/orientation never marshaled and
  the feature enum being ordinals where the JS expects a bitmask).
- `web-host/webxr_bridge.c` — session lifecycle + XR frame pump. Owns
  `VR_GetVRProjection` / `VR_SetHMDOrientation` / `VR_SetHMDPosition`.
  Notable mechanics:
  - **Loop handoff:** `emscripten_pause_main_loop()` on session start, XR
    rAF drives QC_BeginFrame/QC_DrawFrame(eye,x,y)/QC_EndFrame, resume on end.
  - **Shared-layer stereo:** WebXR gives ONE framebuffer with per-eye
    viewport sub-rects (vs OpenXR's two FBOs at 0,0). The per-eye offset
    rides the existing `QC_DrawFrame(eye, x, y)` params →
    `r_refdef.view.x/y`, which places both the 3D view and the 2D/HUD ortho
    stage (gl_rmain.c:5685). Per-eye scissored clear at bridge level.
  - **FBO seam:** library binds the layer framebuffer raw (registered in
    GL.framebuffers, `.name=id`); engine's `R_Mesh_Start()` re-fetches
    `GL_FRAMEBUFFER_BINDING` → `gl_state.defaultframebufferobject` each
    QC_DrawFrame, so engine FBO passes return to the XR target. Validated by
    `web-host/fbo_smoketest.c` (`?fbotest=1`, kept as regression probe).
  - **Projection:** direct memcpy of `XRView.projectionMatrix`; the engine's
    Quake-unit zNear/zFar are pushed via `updateRenderState({depthNear,
    depthFar})` (far quantized to pow2, capped 64k, so per-frame farclip
    fluctuation doesn't churn render state). `r_useinfinitefarclip`'s 1<<23
    farclip is harmless — our memcpy overwrites the engine matrix either way.
  - **Pose:** `QuatToYawPitchRoll` ported from TBXR_Common.c:911-938 (types
    only; identical -z/-x/y remap — WebXR = OpenXR convention). Position
    passed RAW (consumers in view.c remap per-field; do NOT remap in bridge).
    `playerHeight` latched on the screen-layer→VR transition like the
    Android host. Fixed-IPD `GetStereoSeparation()` untouched by design.
  - **Resolution:** first XR frame → `QC_SetResolution(eyeW, eyeH)` (one
    VID_Restart_f); session end → restore canvas size. vid.width/height =
    ONE eye's viewport, not the double-wide layer.
  - M2 aims with the head: `gunangles = hmdorientation` each XR frame
    (controllers are M3).
- `main_web.c` — `VR_UseScreenLayer()` returns false during a session;
  `GetFOV()` derives from the XR projection matrix (culling only); engine
  args now `-userdir /quake_user`; 10s persistence tick.
- **Persistence (M1 issue #4):** `/quake_user` = IDBFS mount (index.html
  preRun, initial `FS.syncfs(true)` gated behind a run dependency so saved
  config is present before `main()`). Engine writes `config.cfg`/saves to
  `/quake_user/id1/` via `-userdir`; `WebHost_PersistTick` saves config +
  syncs every 10s, `_WebHost_PersistNow` flushes on visibilitychange.
  Round-trip verified (config.cfg survives reload pre-tick).
- **index.html** — dedicated "Enter VR" button (fresh user activation,
  separate from Start), `window.onQuakeXRState` UI feedback,
  `?autostart=1` / `?fbotest=1` dev params.

## M2 verification notes

- Headless XR needs a fake device: `iwer` npm package (Immersive Web
  Emulation Runtime), `new XRDevice(metaQuest3).installRuntime({forceInstall:
  true})` injected via evaluateOnNewDocument (forceInstall because headless
  Chrome exposes a native navigator.xr that reports immersive-vr
  unsupported). IWER's layer has `framebuffer: null` (= canvas backbuffer,
  spec-legal) — readback must happen inside an XR rAF registered after the
  library's.
- Known cosmetic: `glCopyTexSubImage2D: Invalid copy texture format` warns
  during VID_Restart loading-plaque capture (RGB canvas → RGBA texture;
  alpha:false context attr). Pre-existing, warning-only.
- The fork's `glsl/default.glsl` **water permutation fails to compile** on
  strict WebGL2 ("No precision specified for (float)") — pre-existing fork
  bug, engine falls back gracefully. Fix in M3/M4 if water looks wrong.

## Build & run

```bash
# build (Git Bash; emsdk auto-activated by the script)
src/build.sh                # SDL audio (default), -j8
src/build.sh --audio=null   # null audio backend (first-boot debugging)
src/build.sh --clean        # wipe build/obj

# serve (COOP/COEP headers, crossOriginIsolated=true)
node web/serve.mjs 8090     # then open http://localhost:8090/
```

Output: `web/quake.js` + `web/quake.wasm` + `web/quake.data` (18.7MB shareware
pak preloaded to MEMFS at `/quake/id1/pak0.pak`).

## Architecture

- `src/darkplaces/` — the QuakeQuest fork's engine, near-pristine (all edits
  marked `WEBXR-PORT`, list below). `vid_android.c` is compiled as-is: it is
  the GLES2 vid layer + the QC_* frame-pump boundary, nothing Android in it.
- `src/web-host/main_web.c` — replaces QuakeQuestSrc (the Android/OpenXR
  host). Creates the WebGL2 context (`emscripten_webgl_create_context`),
  runs `Host_Main()` (= `Host_Init()`, the fork's main loop is already
  inverted), then `emscripten_set_main_loop` pumps
  `QC_BeginFrame(false)` / `QC_DrawFrame(0,0,0)` / `QC_EndFrame()` per rAF —
  the exact protocol `AppThreadFunction()` used on Android
  (QuakeQuest_OpenXR.c:276-310), single eye.
- `src/web-host/sys_web.c` — replaces `sys_linux.c` (Sys_* console/error
  plumbing; `main()` lives in main_web.c).

### Host-provided symbols (the engine links against these)

| Symbol | M1 behavior | M2/M3 plan |
|---|---|---|
| `VR_GetVRProjection(eye,zNear,zFar,m)` | returns false, leaves `m` = engine's own perspective matrix | copy `XRView.projectionMatrix` |
| `VR_UseScreenLayer()` | returns true → zero stereo separation, flat HUD | false during XR session |
| `GetFOV()` | 90 (fork removed scr_fov cvar) | HMD fov_y |
| `GetSysTicrate()` | 1/60 | 1/refresh from XR session |
| `BigScreenMode(mode)` | no-op | drives 2D-quad menu layer |
| `QC_exit(code)` | cancels main loop | + session end |
| `hmdorientation[3]`, `hmdPosition[3]`, `weaponOffset[3]`, `playerHeight` | zeros | fed from WebXR poses |
| `vrMode` (defined in engine's menu.c, default 2) | set to 0 at startup | 1 in VR |

### Input (flatscreen)

- Mouse-look: pointer lock on first canvas click; host accumulates absolute
  yaw/pitch, passes via `QC_MoveEvent(yaw,pitch,0)` with `vr_yawmode 0`
  (absolute-apply mode in `IN_Move`).
- **`gunangles` = view angles each frame** (set in `web_frame` before
  `QC_BeginFrame`). This matters: the fork sends `gunangles`, not viewangles,
  to the server as aim direction (cl_input.c:1845). On Android the VR
  controller wrote it; flatscreen must mirror the view.
- Keyboard: browser keydown/keyup → `QC_KeyEvent` → `Key_Event` (K_* mapping
  in `MapKey()`); WASD/space/mouse binds injected via `Cbuf_AddText` after
  init. Arrow-key turning desyncs from the host-side yaw accumulator (mouse
  is authoritative) — known M1 limitation.
- Cvars forced at init: `cl_trackingmode 0` (classic gun-follows-view).

## Engine file modifications (all marked `WEBXR-PORT` in source)

1. **common.c** — prototype for `portable_vsnprintf` (implicit function
   declarations are hard errors under emscripten clang 21).
2. **glquake.h** — prototypes for the GLES2 forwarders/no-op stubs that
   `vid_android.c` defines (`qglBindFramebuffer`, `glColor4f`, …); previously
   called without prototypes.
3. **dpsoftrast.c** — `#ifndef bool` guard around `typedef qboolean bool;`
   (build force-includes `<stdbool.h>`; the fork's code uses `bool` without
   including it, relying on NDK header leakage).
4. **cl_screen.c** — `R_RenderView(0.0)` → `R_RenderView()`. **wasm traps on
   call-signature mismatches** that native ARM silently tolerated; this one
   spurious argument made *every* `R_RenderView` call in the TU trap.
5. **render.h** — `void R_RenderView(void);` explicit prototype.
6. **cl_screen.h / keys.c** — `SCR_DrawScreen` given its real `(int x, int y)`
   prototype; keys.c call site passed `(0, 0)`. Same wasm signature issue.
7. **snd_sdl.c** — under `__EMSCRIPTEN__`, `SDL_OpenAudio(&wantspec, NULL)`
   so SDL converts to S16 internally (WebAudio device is float32-native;
   the obtained-spec mismatch check rejected AUDIO_F32 and audio never
   started).
8. **vid_shared.c** (M2) — under `__EMSCRIPTEN__`, `VID_Restart_f` restarts
   only the renderer modules, not sound. emscripten SDL2's
   CloseAudio/OpenAudio cycle leaves the old WebAudio ScriptProcessorNode
   firing into freed heap (OOB trap per audio tick) + a stale resume
   listener on a closed AudioContext; VID restarts run on every XR session
   enter/exit, so sound stays up across them.

Nothing else in the engine is modified. `snd_opensl.c` (Android OpenSL) is
simply not compiled; `snd_sdl.c` + the standard snd stack replace it.
`sys_linux.c`, `QuakeQuestSrc/*` are not compiled (replaced by web-host/).

## Build flags of note (src/build.sh)

- `-std=gnu17 -include stdbool.h` — matches NDK-era C semantics; C23 would
  break `qtypes.h`'s `enum {false,true}`.
- `-sMIN/MAX_WEBGL_VERSION=2` + context `majorVersion 2` — WebGL2 accepts the
  engine's GLSL ES 1.00 shaders unmodified.
- `-sFULL_ES2` — **required**: the engine renders fully client-side arrays
  (see below).
- `-sGROWABLE_ARRAYBUFFERS=0` — emscripten 6.0.2 defaults to a resizable wasm
  heap; Chrome's TextDecoder refuses views of resizable buffers, which
  crashed on the first ENOENT `strError()` during fs probing.
- `-sINVOKE_RUN=0` + `Module.callMain()` from the Start button — user
  activation for audio/pointer-lock, per browser autoplay policy.
- `-sUSE_OGG=1 -sUSE_VORBIS=1` — satisfies `LINK_TO_LIBVORBIS` (hardcoded in
  quakedef.h) via emscripten ports.
- `--profiling-funcs` — keep wasm function names; costs little, invaluable
  for stack traces. Drop for a size-optimized release.
- `-sASSERTIONS=1` — still on; drop for release builds.
- No pthreads, no ASYNCIFY (loop already inverted; sv_threaded defaults off).

## The VBO story (important M2 perf item)

WebGL cannot mix client-side vertex arrays with a bound element/index VBO.
Several engine batch paths (`RSurf_PrepareVerticesForBatch` pass-through +
`R_Mesh_VertexPointer` and friends) produce exactly that mix, and
`-sFULL_ES2`'s client-array emulation `abort()`s on it. Enabling the fork's
dynamic-VBO cvars did **not** cover all paths (batch arrays still reached
the GL layer client-side while static index VBOs were bound).

**M1 resolution:** the web host forces `gl_vbo 0`, `gl_vbo_dynamicvertex 0`,
`gl_vbo_dynamicindex 0` (via `Cvar_SetQuick`, before the first frame) —
everything is client-side, FULL_ES2 emulates it correctly (one scratch-VBO
upload per draw). Same operating point as upstream DP-wasm/Qwasm.

**M2 TODO:** port upstream darkplaces' `vid.forcevbo` approach — its
`R_Mesh_*Pointer` functions upload client pointers to a streaming VBO when no
buffer is supplied. That removes the per-draw copy tax and would let
`-sFULL_ES2` be dropped entirely. Do this before Quest-browser perf testing.

## Known issues / M3 handoff

1. **Perf: client-side arrays** (above). Desktop headless Chrome is fine;
   Quest 3 browser will feel it. Fix = forcevbo-style uploads (in progress
   on branch `vbo-modernization` by another agent — do not touch
   R_Mesh_*Pointer paths or gl_vbo/FULL_ES2 flags on master).
2. **Audio latency/underruns not profiled.** SDL_OpenAudio with NULL obtained
   spec adds an SDL-side S16→F32 conversion per callback. Fine on desktop;
   profile on Quest. Audible-output check was not possible headless — code
   path verified ("Sound format: 48000Hz…"), listen on first manual run.
3. **`Host_Mingled: time stepped forward`** warning on first frame (realtime
   starts at epoch-seconds jump). Harmless.
4. **Flatscreen window resize still not wired** (canvas fixed 1280x720).
   The VR enter/exit resolution handoff (M1 issue #3) IS wired; a browser
   window-resize handler for flatscreen remains a nice-to-have (gate it on
   `!WebXRBridge_IsSessionActive()` per design doc risk #6).
5. ~~Config persistence~~ **RESOLVED in M2**: IDBFS at /quake_user (above).
6. **Arrow-key turning vs mouse-yaw desync** (host-side accumulator is
   authoritative). Cosmetic for M1.
7. **`Cmd_AddCommand: menu_reset already defined`** at boot — pre-existing
   fork quirk, harmless.
8. **Networking compiled but inert** (lhnet/libcurl dlopen fails gracefully).
   Out of scope per plan.
9. **JS-side globals**: the emscripten module is non-modularized; the page
   uses `Module.callMain`.
10. **Menus in VR (M3)**: with `VR_UseScreenLayer()` false, the menu/console
    render per-eye at the eye viewports (readable but head-locked). M3's
    in-scene menu quad + d-pad nav replaces this. Menu toggle needs a
    rebind (no Menu button in browser Gamepad API — reports/06).
11. **Water GLSL permutation fails on strict WebGL2** (missing precision
    qualifiers in the fork's default.glsl water path) — logged fallback,
    fix when it visibly matters.
12. **In-headset "Exit VR"**: the page button isn't reachable in-headset;
    Quest's system UI ends the session (wired + tested). An in-game menu
    item can call `WebXRBridge_RequestExit()` in M3.

## M2 starting points (historical — all implemented, see M2 architecture above)

- Frame pump per eye already proven: call `QC_DrawFrame(eye, x, y)` twice
  with `r_stereo_side`-driven projection from `VR_GetVRProjection` returning
  the XRView matrix (transposed to GL column-major — it already is).
- `VR_UseScreenLayer()` false + `GetStereoSeparation()` drive the eye offset
  from `vr_worldscale` — the engine side is intact and untouched.
- Bind the `XRWebGLLayer` framebuffer before each `QC_DrawFrame` (the engine
  draws to whatever FBO is current; it never touches FBO 0 explicitly —
  same seam TBXR_prepareEyeBuffer used).
- Reference: prior-art/emscripten-webxr `library_webxr.js` (XR rAF bypasses
  emscripten_set_main_loop; pause/resume the window loop around sessions).
