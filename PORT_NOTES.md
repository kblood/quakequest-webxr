# PORT_NOTES — M1 (flatscreen) + M2 (WebXR stereo) + M3 (input/gameplay) + M4 (headset-independent fixes)

State as of 2026-07-12: **M1 + M2 complete (pending real-headset confirm);
M3 input foundation in place** (controller poses/buttons/axes/haptics flow
JS→C each XR frame, verified emulated). The four M3 gameplay chunks
(reports/06 §5) build on the contract below.

## M3 foundation — WebXR controller input (the chunk agents' contract)

Files: `web-host/webxr_input.h` (READ THIS FIRST — full contract in its
header comment), `web-host/webxr_input.c`, vendored-library PATCH #12/#13
(`web-host/lib/webxr/`). Everything updates once per XR frame from
`webxr_bridge.c`'s OnXRFrame (before `QC_BeginFrame`, inside the frame
callback); outside a session all state reads zero/inactive.

### Two views of the same per-frame data

1. **Raw** — `WebXRControllerState webxr_controllers[2]` (index 0=left,
   1=right; struct in `lib/webxr/webxr.h`): presence flag, grip pose AND
   targetRay/aim pose (position[3] + orientation quat[4], XR reference-space
   meters, x right / y up / z back), xr-standard gamepad
   `buttons[8]{pressed,touched,value}` + `axes[4]`, `gamepadConnected` /
   `hasHaptic` / `buttonCount` / `axisCount`. Nothing interpreted — note
   axes[3] (thumbstick y) is **+y = down** here, per Gamepad convention.

2. **TBXR-compatible** — the exact globals/names/shapes
   `HandleInput_Default()` consumed on Android, so its logic ports with
   minimal rewording:
   ```c
   WebXRRemoteState leftTrackedRemoteState_new,  leftTrackedRemoteState_old;
   WebXRRemoteState rightTrackedRemoteState_new, rightTrackedRemoteState_old;
   WebXRTrackedController leftRemoteTracking_new, rightRemoteTracking_new;
   ```
   - `WebXRRemoteState` == `ovrInputStateTrackedRemote`: `.Buttons`/`.Touches`
     (same `xrButton_*` bitmask values as TBXR_Common.h, defined in
     webxr_input.h), `.IndexTrigger`/`.GripTrigger` analog 0..1 (bit set at
     the fork's >0.5 threshold), `.Joystick.x/.y` in **OpenXR sign convention
     (+y = pushed forward)** — the xr-standard sign flip already happened,
     do NOT flip again.
   - `WebXRTrackedController` == `ovrTrackedController`: `.Active`,
     `.Pose.position/.orientation` = the **aim pose** (the fork tracked via
     OpenXR aim space; grip is only in the raw view),
     `.Velocity.linearVelocity` in m/s **derived from aim-position deltas**
     (WebXR has no XrSpaceVelocity).
   - The foundation writes `*_new` only. Gameplay code copies new→old at the
     end of its per-hand blocks, exactly as the fork did.

### Quest Touch xr-standard mapping (implemented; re-verify on real headset)

| gamepad index | input | compat bit |
|---|---|---|
| buttons[0] | trigger (analog) | `xrButton_Trigger` (>0.5) |
| buttons[1] | squeeze (analog) | `xrButton_GripTrigger` (>0.5) |
| buttons[3] | thumbstick click | `xrButton_LThumb`/`RThumb` **and** `xrButton_Joystick` |
| buttons[4] | X (left) / A (right) | `xrButton_X`/`xrButton_A` |
| buttons[5] | Y (left) / B (right) | `xrButton_Y`/`xrButton_B` |
| buttons[6] | thumbrest (touch) | `xrButton_ThumbRest` (Touches only) |
| axes[2,3] | thumbstick x,y | `.Joystick` (y sign-flipped) |

**`xrButton_Enter` (Quest Menu button) is NEVER set** — browsers reserve it.
Chunk 3 must rebind the menu toggle (reports/06 §1.4 gap).

### Haptics

- `WebXRInput_Vibrate(durationMs, channelMask, intensity)` — drop-in
  `TBXR_Vibrate` replacement (same signature/semantics: mask 1=left 2=right
  3=both; busy channel rejects the request; -1 = continuous until a
  duration-0 call). Chunk 2's ported `VR_HapticEvent` per-weapon table calls
  this unchanged. Channels tick inside `WebXRInput_Update`.
- `WebXRInput_HapticPulse(hand, intensity, durationMs)` — raw single pulse.
- Both are safe no-ops without an actuator; both are EMSCRIPTEN_KEEPALIVE
  (callable from JS/tests as `Module._WebXRInput_Vibrate(...)`).

### Helpers

- `WebXRInput_GetHMDPositionDelta(out[3])` — the fork's
  `positionDeltaThisFrame` (chunk 1 positional locomotion, chunk 4
  bullet-time). Raw XR axes, meters.
- `WebXRBridge_QuatToYawPitchRoll(q, rotAdjust, out)` (webxr_bridge.h) —
  already ported in M2; chunk 2 uses it for gunangles.

### Debug: `vr_inputdebug` / `?inputdebug=1`

Live input-state dump, ON: `vr_inputdebug 1` in the console, or
`?inputdebug=1` URL param. Two sinks:
- DOM overlay `#qq-inputdebug` (5 Hz) — what the emulated tests assert on
  (the text is formatted in C **from the C-side structs**, so a match proves
  JS→C marshaling);
- `Con_Printf` at 1 Hz — notify lines render in-game per eye, **visible
  inside the headset** for on-device binding diagnosis.
Format per hand: grip/aim positions (`!` = pose not located), aim quat,
derived velocity, compat `btn=`/`tch=` bitmasks + analog triggers + mapped
stick, then raw gamepad (`P`=pressed `t`=touched + value per button, raw
axes).

### M3-foundation verification (2026-07-12, emulated)

`web-host/test/m3-input-test.mjs` (IWER v2.3.0 metaQuest3, headless Chrome —
same harness pattern as M2; reusable by the chunk agents). 24/24 checks OK,
0 console errors: both hands present with valid grip+aim poses; driven
buttons/axes arrive on the C side with correct bits/values (trigger bit+
analog, grip, A/B/X touch+press, thumbstick click setting both thumb bits,
stick x, stick y sign-flip); driven controller positions arrive in the aim
pose; haptic pulse accepted by the emulated actuator; busy-channel rejection;
state clears on button release and resets on session exit; flatscreen
regression OK (webxr-test, 0 errors, crossOriginIsolated=true). Screenshots:
`web/screenshots/m3-input-emulated.png` (stereo + overlay + in-game notify
dump), `m3-flatscreen-regression.png`.

Not verifiable emulated (M3 headset checklist): real Quest Touch button
indices/thumbrest, actual rumble feel, `targetRayMode`
differences, hand-tracking input sources (foundation matches by handedness
and ignores non-controller sources' missing gamepads gracefully).
Engine links, boots shareware Quake flatscreen at 1280x720; entering VR starts
an immersive-vr session, pauses the flatscreen loop, and renders head-tracked
per-eye stereo into the XRWebGLLayer framebuffer. Config/saves persist via
IDBFS. Verified headless incl. a full emulated XR session (IWER): enter →
stereo render → exit → re-enter, zero console/page errors.

## M3 gameplay — merged (integration pass 2026-07-12, reports/09-m3-integration.md)

All four M3 chunks (locomotion `in_locomotion.c`, weapon `in_weapon.c`,
hud/menus `vr_menu_quad.c`+`in_menu.c`, comfort `in_comfort.c`) are merged.
Per-frame dispatch order (fixed, do not reorder casually):
`WebXRInput_Update` → `IN_Weapon_Update` (sets gunangles for the frame) →
haptics/debug → `WebXRLoco_Update` (reads gunangles); then in OnXRFrame:
`IN_Menu_HandleInput` → `VRMenuQuad_RunFrame` (early-returns on 2D-UI
frames) → `WebXRComfort_Update` → head-aim gunangles fallback → frame pump.
Note `WebXRComfort_Update` sits after the quad early-return by design:
quicksave/quickload/bullet-time are gameplay actions and stay inert while a
menu/console/demo is up.

### Merged VR binding map (default cl_righthanded 1)

| Input | Action | Owner |
|---|---|---|
| Dominant (R) trigger | fire (`+attack`) | in_weapon |
| Off-hand (L) trigger | run (`+speed`); fire when left-handed | in_locomotion |
| Right A (in-game) | jump (K_SPACE, fork :863-866; headset-QA bug-1 fix — was unported) | in_locomotion |
| Right B (in-game) | *nothing* — explicitly `//Unused` in the fork (:869-874) | — |
| Dominant (R) thumbstick click | laser-sight cycle | in_weapon |
| Off-hand (L) thumbstick click SHORT (<600 ms) | menu toggle (K_ESCAPE dn+up on release) | in_menu |
| Off-hand (L) thumbstick click LONG (≥600 ms, in-game) | recenter + haptic confirm | in_menu → in_comfort |
| Right stick Y flick (±0.7) | weapon prev/next (`/`=impulse 10, `#`=impulse 12, binds injected by IN_Weapon_Init) | in_weapon |
| Right stick X | snap/stick turn (vr_yawmode) | in_locomotion |
| Left stick | smooth locomotion (deadzone+curve) | in_locomotion |
| Off-hand grip + hands <0.5 m | two-handed stabilization | in_weapon |
| Left X / Y (always physical left) | quicksave / quickload | in_comfort |
| Both sticks + A/B while menu up | d-pad nav / select / back | in_menu |

Deliberate deviations from the fork's HandleInput_Default, re-audited for the
bug-1 fix (headset QA 2026-07-12): the fork's `canUseQuickSave` is never set
true anywhere in its tree, so its X/Y quicksave/quickload path was DEAD CODE
(X = god-mode debug, Y = VR text-input toggle in the shipped build); the port
enables the quicksave path on purpose. The fork's thumbstick VR text-input
keyboard (`textInput` mode, :650-744) is not ported — browser text entry can
use the real keyboard, and Y is quickload here. With the jump fix above,
every LIVE in-game binding of the fork now has a port-side owner.

### Integration fixes (found only in the merged build)

1. **Off-hand thumbstick-click collision**: chunk 3 bound MENU TOGGLE and
   chunk 4 bound RECENTER to the same click — both reports independently
   called it "the one input the fork left free". In the merge the click
   opened the menu AND (on the closing click, via stale edge state across
   the quad path's early return) fired a spurious recenter. Resolution:
   `in_menu.c` is the single owner of the gesture — short press toggles the
   menu (fires on release; <1-frame cost), long press (≥600 ms) recenters
   with a 150 ms haptic confirm, disabled while the menu quad is up (a
   recenter would yank the quad anchor; release just closes the menu).
   Menu keeps the plain click per chunk 3's rationale; `vr_recenter`
   console command remains.
2. **`?args=` argv passthrough upstreamed to `web/index.html`**
   (space-separated; `?startargs=` comma form kept for the comfort
   harness) — was previously per-worktree test-copy only (08b issue 6).
   Useful in-headset too (`?args=+map e1m2`).
3. Weapon-switch binds `/` + `#` (08d flagged the shareware default.cfg
   only binds `/`): verified IN_Weapon_Init injects BOTH at init, after
   config.cfg exec — both flick directions work in the merged build.

## M4 — headset-independent fixes (2026-07-12)

### Water rendering on strict WebGL2 (known issue 11 — RESOLVED)

Root cause (from live browser console capture): the fork's water/refraction
GLSL builds its fragment shader without a default float precision —
GLSL ES 1.00 fragment shaders have NONE, and desktop ANGLE enforces that
strictly ("No precision specified for (float)" at the MODE_WATER locals),
while Android GLES drivers were lenient. Three-part fix, all
`__EMSCRIPTEN__`-guarded (Android/desktop behavior unchanged):

1. **gl_rmain.c** `R_GLSL_CompilePermutation`: inject a
   `precision highp/mediump float;` pretext line into fragment shaders (with
   matching blank lines in vert/geom stages to keep line numbers aligned).
2. **vid_android.c** `GLES_Init`: advertise FBO + NPOT support — WebGL2 has
   framebuffer objects and NPOT textures in core (the GLES2 path probed for
   `GL_OES_texture_npot`, which WebGL2 never advertises). This turns the
   water FBO path (`usewaterfbo`) on; without it the fallback
   `R_Mesh_CopyToTexture` path spams `glCopyTexSubImage2D` INVALID_OPERATION
   (RGB backbuffer → RGBA texture) every frame.
3. **gl_textures.c**: WebGL2 renderbuffers/depth textures need SIZED internal
   formats — `GL_DEPTH24_STENCIL8` etc. instead of unsized
   `GL_DEPTH_COMPONENT` in the GLES2 textype table.

Verified: 0 console warnings across a full demo run, water visibly renders
(web/screenshots/m4-water-fixed.png), m3-integration-test 18/18 (the XR-layer
FBO seam is untouched — R_Mesh_Start refetches GL_FRAMEBUFFER_BINDING each
frame). **Headset QA note:** this newly enables the water-FBO path on the
Quest browser too — spot-check water perf + rendering in an XR session.

### Full-game data drop-in (browser-local pak upload)

`web/index.html` grew a file-picker (`#pak-input`, accept .pak/.pk3) that
writes user-supplied pak files into `/quake_user/id1/` — the IDBFS mount
`-userdir` points at, so they persist in IndexedDB across reloads and
`FS_AddGameHierarchy` puts the userdir AHEAD of the preloaded shareware pak.
**Client-side only**: bytes go browser-local, nothing is ever uploaded (the
UI says so explicitly). While the engine is running, the page calls the new
`WebHost_RescanFS()` export (main_web.c) → `fs_rescan`, which rebuilds the
search path and re-checks `gfx/pop.lmp` to flip `registered`. Notes:

- Vanilla DP `FS_Rescan` only ever SETS `registered`, never resets it to 0 —
  removing paks live keeps "registered" until reload (UI mentions it).
- **GOG "Quake Enhanced" (KEX) pak0.pak WORKS**: standard PACK, 1121 entries,
  contains gfx/pop.lmp; 172 MB stores fine, e1m1 boots with remastered
  assets, QC runs clean. Caveats: menu/HUD strings referencing `$qc_*`
  localization keys show placeholder text (translations live in QuakeEX.kpf,
  which DP doesn't load) and harmless `CVAR_SET: VARIABLE CAMPAIGN NOT
  FOUND` notifies. Classic pak0.pak+pak1.pak give correct text; KEX is a
  playable alternative — no hard "classic data required".
- Harness: `web-host/test/m4-gamedata-test.mjs` (11 checks, synthetic
  registered-marker pak; optional `QQ_KEX_PAK=<path>` compat recon).

### cl_trackingmode config stomp (reports/08b issue 3 — RESOLVED)

See in_weapon.c "cl_trackingmode: runtime value vs saved preference": the
user preference is latched at init and on any non-forced cvar change; VR
entry applies the preference (not a hardcoded 1); flatscreen forces 0 at
runtime only; `IN_Weapon_SaveConfigPreservingTrackingMode()` swaps the
preference in around Host_SaveConfig so config.cfg never records the forced
value. Harness: `web-host/test/m4-trackingmode-test.mjs` (15 checks).

## Headset QA round 1 (2026-07-12) — bug fixes

Three bugs from real-Quest QA. Bug 1 (jump unbound) is documented in the
M3 binding map above (Right A row + "canUseQuickSave" deviation note).

### Bug 3: exit VR bricked re-entry + pointer-lock Esc synth (RESOLVED)

Root cause: the vendored library's session `'end'` listener opened with
`Module['webxr_session'].cancelAnimationFrame(WebXR._curRAF)` — an rAF call
on the *already-ended* session. Original-spec WebXR UAs throw
`InvalidStateError` there (current Chromium/IWER made it a no-op, which is
why plain emulation didn't reproduce it), so the listener died before the C
`onSessionEnd` ever ran: `s_sessionActive` stuck true → `RequestSession`
early-returns forever (no re-entry) and every `!WebXRBridge_IsSessionActive()`
guard stays suppressed (the pointer-lock-exit → Esc menu synthesis dies) —
both QA symptoms from one stuck flag. Fix layers:

- **PATCH #15** (`lib/webxr/library_webxr.js`, see PATCHES.md): every step of
  the `'end'` listener individually guarded so `onSessionEnd` ALWAYS reaches
  C; `onFrame` rAF re-arm and `webxr_request_exit`'s `end()` also guarded.
- **webxr_bridge.c teardown reorder**: `OnSessionEnd` resumes the main loop
  and notifies the page BEFORE the GL-heavy `QC_SetResolution`/VID_Restart,
  so a fault there can't leave a paused loop + stale button.
- **Self-heal**: `WebXRBridge_IsSessionActive()` reconciles a stale true flag
  against JS truth (`Module['webxr_session']` gone → force end teardown);
  `WebXRBridge_RequestSession()` on a "still active" click also treats >2 s
  without an XR frame as a dead session (a 2D-page click can't happen while
  a healthy immersive session presents), force-cleans, and proceeds.
- **web/index.html**: Enter/Exit button now consults C-side truth
  (`_WebXRBridge_IsSessionActive`) instead of the page's `xrActive` mirror,
  and no longer crashes if clicked before the wasm runtime is ready.

Regression harness: `web-host/test/m4-session-cycle-test.mjs` — full
enter→exit→re-enter→exit cycle (C flag, page notification, flatscreen loop
resumption via new `WebHost_FrameCount` export, pointer-lock Esc synth, both
eyes lit and stereo-distinct on every entry) in three UA flavors: plain IWER
(`canvas`), IWER + real layer framebuffer shim (`layerfb`, device-like), and
IWER + original-spec throwing rAF shim (`endquirk`, reproduces this bug —
hung at exit#1 pre-fix, 72/72 checks green post-fix, 0 console errors).

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
9. **gl_rmain.c** (M4) — under `__EMSCRIPTEN__`, inject a default fragment
   float precision pretext into GLSL compiles (water fix, see M4 section).
10. **vid_android.c** (M4) — under `__EMSCRIPTEN__`, advertise FBO + NPOT
    support in `GLES_Init` (core in WebGL2; the extension strings probed on
    Android never appear).
11. **gl_textures.c** (M4) — under `__EMSCRIPTEN__`, sized depth/stencil
    internal formats in the GLES2 textype table (WebGL2 requirement).

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
11. ~~Water GLSL permutation fails on strict WebGL2~~ **RESOLVED in M4**
    (missing default fragment precision + FBO/NPOT support flags + sized
    depth formats — see the M4 section above).
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
