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
