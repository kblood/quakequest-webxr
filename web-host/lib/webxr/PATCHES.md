# Vendored emscripten-webxr — patch log

Upstream: https://github.com/VhiteRabbit/emscripten-webxr @ `1bc0b7b`
("Delay onSessionStart until baseLayer and first refSpace are available"),
MIT license (COPYING). Vendored per reports/05-webxr-bridge-design.md §1/§7.

Patches #1, #1b, #2 are the three bugs the design doc identified. #3 is the
design doc's §7 item 4. The rest were found while making the library actually
compile/run under emscripten 6.0.2 + plain C, each documented below.

## #1a — `toFeatureList` returned the unfiltered constant (library_webxr.js)

Design doc §1 bug 1. The closure built the filtered array `f` from the bitmask
but did `return features;`. Every session request therefore required all five
features **including `hit-test`**, which is not guaranteed for `immersive-vr`
and can turn "Enter VR" into a silent `NotSupportedError`. Fix: `return f;`.

## #1b — `webxr_request_session` dropped its feature arguments (library_webxr.js)

Design doc §1 bug 1 (second half). The exported stub only forwarded `mode`;
`requiredFeatures`/`optionalFeatures` (declared in webxr.h) never reached the
session request — combined with #1a, feature selection was entirely
non-functional. Fix: forward all three parameters.

## #2 — reference-space selection race (library_webxr.js)

Design doc §1 bug 2. Upstream fired `requestReferenceSpace` for
local/local-floor/bounded-floor/unbounded **in parallel**; every resolution
overwrote `WebXR.refSpace`, so the *last* promise to resolve won —
nondeterministic floor-height origin per run. Fix: sequential fallback chain
`['local-floor', 'bounded-floor', 'local', 'unbounded']`; first supported
space wins deterministically. ('viewer' still bootstraps the first rAF.)

## #3 — stale `Module.webxr_fbo` on session end (library_webxr.js)

Design doc §7 item 4 / risk #10. The session `end` handler never cleared
`Module.webxr_fbo`/`GL.framebuffers[id]`, so re-entering VR reused a stale GL
id-table slot. Fix: null both in the `end` listener.

## #4 — head pose only marshaled the matrix (library_webxr.js)

`onFrame` nativized only `pose.transform.matrix` (16 floats) into the buffer
passed as `WebXRRigidTransform* headPose` — `headPose->position` and
`headPose->orientation` were **uninitialized heap garbage**. The bridge feeds
exactly those fields to `VR_SetHMDPosition`/`QuatToYawPitchRoll`. Fix:
nativize the full rigid transform (matrix + position + orientation).
(Also renamed the local from `modelMatrix` to `headPose` to match webxr.h.)

## #5 — `Module.ctx` → `GLctx` (library_webxr.js)

Upstream assumed the legacy SDL/`Browser.createContext` path which sets
`Module.ctx`. This port creates its context via the html5 API
(`emscripten_webgl_create_context`), which never sets `Module.ctx` —
`makeXRCompatible`/`bindFramebuffer` would throw on undefined. Fix: use the
`GLctx` library global (the current context) at all four sites.

## #6 — `_set_input_callback` referenced an undefined variable (library_webxr.js)

The select/selectstart/selectend handler used `i` (never declared —
ReferenceError on the first controller event) and malloc'd 8 bytes for the
12-byte 3×i32 input-source struct (heap overflow). Fix: derive the index via
`indexOf(e.inputSource)` and malloc 12. (M3-facing; patched now while the
diff against upstream is fresh.)

## #7 — explicit `__deps` for emscripten 6.x (library_webxr.js)

`setValue`/`getValue`/`GL` are DCE-able JS library symbols in emscripten 6;
a library file referencing them without `__deps` fails at link. Added
`$WebXR__deps: ['$setValue', '$getValue', '$GL', '$dynCall']` (autoAddDeps
propagates `$WebXR` to every entry point). No behavior change.

## #8 — `webxr_is_session_supported` always reported "supported" (library_webxr.js)

`navigator.xr.isSessionSupported()` **resolves with a boolean**; it does not
reject for "unsupported". Upstream reported 1 whenever the promise resolved,
i.e. on every browser exposing `navigator.xr`, regardless of the answer. Fix:
report the resolved boolean.

## #9 — deprecated `session.depthNear/depthFar` setters (library_webxr.js)

Design doc §4.1 note. Direct assignment is the pre-spec API and is silently
ignored by current browsers; the spec routes depth params through
`XRSession.updateRenderState({depthNear, depthFar})`. The bridge relies on
this to bake the engine's Quake-unit near/far into `XRView.projectionMatrix`,
so a silently-ignored setter would leave a 0.1/1000 meter-scale far plane —
world clipped at ~26 map units... i.e. visibly broken. Fix: updateRenderState.

## #10 — webxr.h did not compile as C

- The `extern "C"` guard emitted a bare file-scope `{` in plain C (syntax
  error) and never closed it even in C++.
- Enum tag names were used as bare type names (C++ only) — typedef'd.
- `webxr_get_input_pose` had a C++ default argument — removed.

## #12 — controller/gamepad snapshot API (library_webxr.js + webxr.h) [M3, additive]

Upstream exposed controller *poses* (`webxr_get_input_pose`) and select
events, but no way to read `XRInputSource.gamepad` (xr-standard buttons/axes/
haptics presence) from C. Added `webxr_get_controller_state(handedness, out)`:
zeroes then fills a `WebXRControllerState` (196 bytes, all 4-byte scalars,
layout hand-marshaled in JS — keep webxr.h and the JS offsets in sync) with
per-hand presence, grip pose, targetRay (aim) pose, and up to 8 gamepad
buttons (pressed/touched/value) + 4 axes. Poses require the live XRFrame
(frame-callback only); the gamepad part works whenever a session exists.
Consumed by `web-host/webxr_input.c` (M3 input foundation).

## #13 — haptic pulse (library_webxr.js + webxr.h) [M3, additive]

Upstream had no haptics path at all. Added `webxr_haptic_pulse(handedness,
intensity, durationMs)` → `gamepad.hapticActuators[0].pulse()` with a
`playEffect('dual-rumble', ...)` fallback and a safe no-op (returns 0) when
there is no session/hand/actuator (e.g. emulated runtimes). Backs the
TBXR_Vibrate-compatible channel logic in `web-host/webxr_input.c`.

## #11 — WebXRSessionFeatures values were indices, not bits (webxr.h)

Upstream declared the feature enum as ordinals (LOCAL=0, LOCAL_FLOOR=1, …)
while library_webxr.js (and upstream's own README example, which passes
`WEBXR_SESSION_FEATURE_LOCAL_FLOOR` as a mask) tests `bitMask & (1 << i)`.
Requesting LOCAL_FLOOR (=1) actually requested "local", and LOCAL (=0)
requested nothing. Values changed to the bits the JS tests:
LOCAL=1, LOCAL_FLOOR=2, BOUNDED_FLOOR=4, UNBOUNDED=8, HIT_TEST=16.

## #14 — manual recenter (library_webxr.js + webxr.h) [M3 comfort, additive]

Upstream had no recenter API at all. Added `webxr_recenter()`: replaces
`WebXR.refSpaces[WebXR.refSpace]` with `cur.getOffsetReferenceSpace(offset)`,
where `offset` is built from the current viewer pose's position (X/Z only —
Y stays 0 so `local-floor` height is untouched) and a yaw-only "twist"
extraction of its orientation (so recenter can't tilt the world). Because
both head pose (`onFrame`'s `getViewerPose`) and controller poses
(`webxr_get_controller_state`'s `ref`) read the *same* refSpace object, the
new origin applies consistently to head and both hands with no extra
bookkeeping needed on the C side (the gun's controller-minus-head math stays
correct across a recenter). This is the WebXR-native replacement for the
fork's `TBXR_Recenter` (TBXR_Common.c:1818-1860, which rebuilds the OpenXR
reference space) — except the fork only ever called it reactively from a
runtime recenter event; this is additionally exposed as an explicit in-game
action (`web-host/in_comfort.c` binds it to the off-hand thumbstick click
and the `vr_recenter` console command). Must run inside the frame callback
(needs the live XRFrame for `getViewerPose`). Wrapped in try/catch: a
malformed/zero quaternion (a headset reporting a degenerate pose) falls back
to a no-op (returns 0) instead of throwing out of the frame callback.

## #15 — exception-proof session teardown (library_webxr.js) [bug 3]

Upstream's session `'end'` listener began with
`Module['webxr_session'].cancelAnimationFrame(WebXR._curRAF)` — a call on the
*already-ended* session. The original WebXR spec made `requestAnimationFrame`
/`cancelAnimationFrame` throw `InvalidStateError` on ended sessions (later
relaxed to a no-op, which is what current Chromium and IWER implement), so on
any UA with the original semantics the listener died on its first statement:
`Module['webxr_session']` was never nulled and — critically — the C
`onSessionEnd` callback never ran. The C side then believed the session was
active forever: `WebXRBridge_RequestSession` early-returned (VR could never
be re-entered) and every `!WebXRBridge_IsSessionActive()` guard (e.g. the
pointer-lock-exit → Esc menu synthesis in `main_web.c`) stayed suppressed —
exactly headset-QA bug 3. The call was also a plain bug even on lenient UAs:
`WebXR._curRAF` is initialised to `null` and never assigned anywhere, so it
cancelled nothing. Changes:

- `'end'` listener: `cancelAnimationFrame` only attempted when a handle
  exists, wrapped in try/catch; the PATCH #3 fbo cleanup wrapped in its own
  try/catch; `onSessionEnd(mode)` therefore always reaches the C side.
- `onFrame`'s rAF re-arm wrapped in try/catch (a late frame delivered around
  `end()` would throw on original-spec UAs).
- `webxr_request_exit`: `end()` on an already-ended session returns a
  rejected promise (or throws synchronously on some UAs) — both swallowed.

Belt-and-braces on the C side (webxr_bridge.c, not part of this file but the
same fix): `WebXRBridge_IsSessionActive()` reconciles a stale `true` flag
against `Module['webxr_session']` and forces the end teardown if the JS
session is gone; `WebXRBridge_RequestSession()` additionally treats a
session that stopped delivering XR frames >2 s ago as dead (a click on the
2D page can't happen while a healthy immersive session is presenting) and
force-cleans before requesting the new session. Regression-covered by
`test/m4-session-cycle-test.mjs` mode `endquirk`, which shims IWER's
XRSession to the original-spec throwing behavior.

**Known emulation-only limitation (does not affect real headsets):** IWER
2.3.0's `XRReferenceSpace.getOffsetReferenceSpace` deviates from the WebXR
spec — its own type declaration (`iwer/lib/spaces/XRReferenceSpace.d.ts:26`,
`getOffsetReferenceSpace(originOffset: mat4)`) and implementation
(`iwer/lib/spaces/XRSpace.js:17`, `offsetMatrix ? mat4.clone(offsetMatrix) :
mat4.create()`) expect a raw 16-element `mat4`, not the spec-mandated
`XRRigidTransform` every real browser (including Quest Browser) accepts.
Passing a real `XRRigidTransform` — the only spec-correct input, and what
this function does — makes `mat4.clone` read non-existent numeric indices
off it, producing a NaN `offsetMatrix`; IWER's `mat4.invert` then returns
`null` on that singular matrix without touching its output buffer, so the
(reused, module-scope) scratch matrix silently keeps its previous frame's
value and the emulated pose reads as unchanged instead of erroring. Net
effect: under `test/m3-comfort-test.mjs`'s IWER harness, `webxr_recenter()`
runs to completion, returns success, and the C-side effects it drives
(`SCR_CenterPrint`, `playerHeight` re-latch, the console log line) all fire
correctly — but the emulated headset's *reported pose* does not actually
move to the new origin, so the harness cannot assert the numeric zeroing
end-to-end. Verified independently that the transform this function builds
(position X/Z untransformed + Y zeroed, yaw-only twist quaternion) is the
mathematically correct WebXR "reset pose" idiom via a standalone
gl-matrix-based check (not committed here — ad hoc verification), reproducing
IWER's own `calculateGlobalOffsetMatrix`/`getViewerPose` algebra by hand: a
level head recenters to exactly (0,y,0)/yaw 0, and a tilted head (pitch
12°/roll -7°) recenters to (0,y,0) with <1° of yaw residual (expected
swing/twist coupling, not an error). Real-headset QA should still confirm
the on-device behavior before this ships, since this specific mechanism
(`getOffsetReferenceSpace`) is untestable end-to-end under this emulator
version.
