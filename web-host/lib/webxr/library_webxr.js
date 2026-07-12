/*
 * library_webxr.js — vendored from emscripten-webxr @ 1bc0b7b
 * (https://github.com/VhiteRabbit/emscripten-webxr, MIT — see COPYING).
 *
 * Modified for the QuakeQuest->WebXR port. Every deviation from upstream is
 * marked "WEBXR-PORT PATCH #n" and documented in PATCHES.md next to this file.
 */
var LibraryWebXR = {

/* WEBXR-PORT PATCH #7: explicit runtime deps for emscripten 6.x DCE
 * (setValue/getValue/GL are JS library symbols now and must be declared). */
$WebXR__deps: ['$setValue', '$getValue', '$GL', '$dynCall'],
$WebXR: {
    refSpaces: {},
    _curRAF: null,

    _nativize_vec3: function(offset, vec) {
        setValue(offset + 0, vec.x, 'float');
        setValue(offset + 4, vec.y, 'float');
        setValue(offset + 8, vec.z, 'float');

        return offset + 12;
    },

    _nativize_vec4: function(offset, vec) {
        WebXR._nativize_vec3(offset, vec);
        setValue(offset + 12, vec.w, 'float');

        return offset + 16;
    },

    _nativize_matrix: function(offset, mat) {
        for (var i = 0; i < 16; ++i) {
            setValue(offset + i*4, mat[i], 'float');
        }

        return offset + 16*4;
    },

    _nativize_rigid_transform: function(offset, t) {
        offset = WebXR._nativize_matrix(offset, t.matrix);
        offset = WebXR._nativize_vec3(offset, t.position);
        offset = WebXR._nativize_vec4(offset, t.orientation);

        return offset;
    },

    /* Sets input source values to offset and returns pointer after struct */
    _nativize_input_source: function(offset, inputSource, id) {
        var handedness = -1;
        if(inputSource.handedness == "left") handedness = 0;
        else if(inputSource.handedness == "right") handedness = 1;

        var targetRayMode = 0;
        if(inputSource.targetRayMode == "tracked-pointer") targetRayMode = 1;
        else if(inputSource.targetRayMode == "screen") targetRayMode = 2;

        setValue(offset, id, 'i32');
        offset +=4;
        setValue(offset, handedness, 'i32');
        offset +=4;
        setValue(offset, targetRayMode, 'i32');
        offset +=4;

        return offset;
    },

    _set_input_callback__deps: ['$dynCall'],
    _set_input_callback: function(event, callback, userData) {
        var s = Module['webxr_session'];
        if(!s) return;
        if(!callback) return;

        s.addEventListener(event, function(e) {
            /* Nativize input source */
            var inputSource = Module._malloc(12); /* 3*sizeof(int32) */
            /* WEBXR-PORT PATCH #6: upstream referenced an undefined variable
             * `i` here (ReferenceError on first select event) and malloc'd 8
             * bytes for a 12-byte struct. Use the real index of the source. */
            var i = Array.prototype.indexOf.call(s.inputSources, e.inputSource);
            WebXR._nativize_input_source(inputSource, e.inputSource, i);

            /* Call native callback */
            dynCall('vii', callback, [inputSource, userData]);

            _free(inputSource);
        });
    },

    _set_session_callback__deps: ['$dynCall'],
    _set_session_callback: function(event, callback, userData) {
        var s = Module['webxr_session'];
        if(!s) return;
        if(!callback) return;

        s.addEventListener(event, function() {
            dynCall('vi', callback, [userData]);
        });
    }
},

webxr_init__deps: ['$dynCall'],
webxr_init: function(frameCallback, startSessionCallback, endSessionCallback, errorCallback, userData) {
    function onError(errorCode) {
        if(!errorCallback) return;
        dynCall('vii', errorCallback, [userData, errorCode]);
    };

    function onSessionEnd(mode) {
        if(!endSessionCallback) return;
        mode = {'inline': 0, 'immersive-vr': 1, 'immersive-ar': 2}[mode];
        dynCall('vii', endSessionCallback, [userData, mode]);
    };

    function onSessionStart(mode) {
        if(!startSessionCallback) return;
        mode = {'inline': 0, 'immersive-vr': 1, 'immersive-ar': 2}[mode];
        dynCall('vii', startSessionCallback, [userData, mode]);
    };

    const SIZE_OF_WEBXR_VIEW = (16 + 3 + 4 + 16 + 4)*4;
    const views = Module._malloc(SIZE_OF_WEBXR_VIEW*2 + (16 + 4 + 3)*4);

    function onFrame(time, frame) {
        if(!frameCallback) return;
        /* Request next frame */
        const session = frame.session;
        /* RAF is set to null on session end to avoid rendering */
        if(Module['webxr_session'] != null) session.requestAnimationFrame(onFrame);

        const pose = frame.getViewerPose(WebXR.refSpaces[WebXR.refSpace]);
        if(!pose) return;

        const glLayer = session.renderState.baseLayer;
        pose.views.forEach(function(view) {
            const viewport = glLayer.getViewport(view);
            let offset = views + SIZE_OF_WEBXR_VIEW*(view.eye == 'right' ? 1 : 0);
            offset = WebXR._nativize_rigid_transform(offset, view.transform);
            offset = WebXR._nativize_matrix(offset, view.projectionMatrix);

            setValue(offset + 0, viewport.x, 'i32');
            setValue(offset + 4, viewport.y, 'i32');
            setValue(offset + 8, viewport.width, 'i32');
            setValue(offset + 12, viewport.height, 'i32');
        });

        /* Head pose.
         * WEBXR-PORT PATCH #4: upstream only nativized pose.transform.matrix
         * (16 floats) into a buffer typed as WebXRRigidTransform*, leaving
         * headPose->position/orientation as uninitialized heap garbage. The
         * bridge needs exactly those fields (VR_SetHMDPosition + the
         * QuatToYawPitchRoll port), so nativize the full rigid transform. */
        const headPose = views + SIZE_OF_WEBXR_VIEW*2;
        WebXR._nativize_rigid_transform(headPose, pose.transform);

        /* If framebuffer is non-null, compositor is enabled and we bind it.
         * If it's null, we need to avoid this call otherwise the canvas FBO is bound */
        if(glLayer.framebuffer) {
            /* Make sure that FRAMEBUFFER_BINDING returns a valid value.
             * For that we create an id in the emscripten object tables
             * and add the frambuffer */
            const id = Module.webxr_fbo || GL.getNewId(GL.framebuffers);
            glLayer.framebuffer.name = id;
            GL.framebuffers[id] = glLayer.framebuffer;
            Module.webxr_fbo = id;
            /* WEBXR-PORT PATCH #5: GLctx, not Module.ctx (see PATCHES.md) */
            GLctx.bindFramebuffer(GLctx.FRAMEBUFFER, glLayer.framebuffer);
        }

        /* Set and reset environment for webxr_get_input_pose calls */
        Module['webxr_frame'] = frame;
        dynCall('viiiii', frameCallback, [userData, time, headPose, views, pose.views.length]);
        Module['webxr_frame'] = null;
    };

    function onSessionStarted(session, mode) {
        Module['webxr_session'] = session;

        // React to session ending
        session.addEventListener('end', function() {
            Module['webxr_session'].cancelAnimationFrame(WebXR._curRAF);
            WebXR._curRAF = null;
            Module['webxr_session'] = null;
            /* WEBXR-PORT PATCH #3: drop the stale GL.framebuffers entry for
             * the layer framebuffer so a later re-enter can't alias it
             * (design doc risk #10). */
            if (Module.webxr_fbo != null) {
                GL.framebuffers[Module.webxr_fbo] = null;
                Module.webxr_fbo = null;
            }
            onSessionEnd(mode);
        });

        // Ensure our context can handle WebXR rendering
        /* WEBXR-PORT PATCH #5: GLctx, not Module.ctx. The Emscripten html5
         * WebGL API (emscripten_webgl_create_context) never sets Module.ctx;
         * upstream assumed the old SDL/Browser.createContext path. */
        GLctx.makeXRCompatible().then(function() {
            // Create the base layer
            const layer = Module['webxr_baseLayer'] = new window.XRWebGLLayer(session, GLctx, {
                framebufferScaleFactor: Module['webxr_framebuffer_scale_factor'],
            });
            session.updateRenderState({ baseLayer: layer });

            /* 'viewer' reference space is always available. */
            session.requestReferenceSpace('viewer').then(refSpace => {
                WebXR.refSpaces['viewer'] = refSpace;

                WebXR.refSpace = 'viewer';

                // Give application a chance to react to session starting
                // e.g. finish current desktop frame.
                onSessionStart(mode);

                // Start rendering
                session.requestAnimationFrame(onFrame);
            });

            /* WEBXR-PORT PATCH #2: upstream requested all four spaces IN
             * PARALLEL and let every resolved promise overwrite
             * WebXR.refSpace — the winner was whichever promise resolved
             * LAST (nondeterministic), not the preferred one. Chain them
             * sequentially so priority order is real: the first supported
             * space in this list wins and the chain stops. */
            function tryRefSpace(candidates, i) {
                if (i >= candidates.length) return;
                session.requestReferenceSpace(candidates[i]).then(refSpace => {
                    WebXR.refSpaces[candidates[i]] = refSpace;
                    WebXR.refSpace = candidates[i];
                }, function() { tryRefSpace(candidates, i + 1); });
            }
            tryRefSpace(['local-floor', 'bounded-floor', 'local', 'unbounded'], 0);
        }, function() {
            onError(-3);
        });
    };

    if(navigator.xr) {
        Module['webxr_request_session_func'] = function(mode, requiredFeatures, optionalFeatures) {
            if(typeof(mode) !== 'string') {
                mode = (['inline', 'immersive-vr', 'immersive-ar'])[mode];
            }

            let toFeatureList = function(bitMask) {
                const f = [];
                const features = ['local', 'local-floor', 'bounded-floor', 'unbounded', 'hit-test'];
                for(let i = 0; i < features.length; ++i) {
                    if((bitMask & (1 << i)) != 0) {
                        f.push(features[i]);
                    }
                }
                /* WEBXR-PORT PATCH #1a: upstream returned the unfiltered
                 * `features` constant here, so EVERY session request required
                 * all five features incl. hit-test — a NotSupportedError on
                 * runtimes without hit-test for immersive-vr. */
                return f;
            };
            if(typeof(requiredFeatures) === 'number') {
                requiredFeatures = toFeatureList(requiredFeatures);
            }
            if(typeof(optionalFeatures) === 'number') {
                optionalFeatures = toFeatureList(optionalFeatures);
            }
            navigator.xr.requestSession(mode, {
                requiredFeatures: requiredFeatures,
                optionalFeatures: optionalFeatures
            }).then(function(s) {
                onSessionStarted(s, mode);
            }).catch(console.error);
        };
    } else {
        /* Call error callback with "WebXR not supported" */
        onError(-2);
    }
},

webxr_is_session_supported__deps: ['$dynCall'],
webxr_is_session_supported: function(mode, callback) {
    if(!navigator.xr) {
        /* WebXR not supported at all */
        dynCall('vii', callback, [mode, 0]);
        return;
    }
    navigator.xr.isSessionSupported((['inline', 'immersive-vr', 'immersive-ar'])[mode]).then(function(supported) {
        /* WEBXR-PORT PATCH #8: isSessionSupported RESOLVES with a boolean —
         * it does not reject for "unsupported". Upstream reported 1 whenever
         * the promise resolved, i.e. always "supported" on any browser with
         * navigator.xr. Report the actual boolean. */
        dynCall('vii', callback, [mode, supported ? 1 : 0]);
    }, function() {
        dynCall('vii', callback, [mode, 0]);
    });
},

/* WEBXR-PORT PATCH #1b: upstream dropped requiredFeatures/optionalFeatures
 * on the floor here (only forwarded `mode`), even though webxr.h declares
 * all three parameters. Forward them. */
webxr_request_session: function(mode, requiredFeatures, optionalFeatures) {
    var s = Module['webxr_request_session_func'];
    if(s) s(mode, requiredFeatures, optionalFeatures);
},

webxr_request_exit: function() {
    var s = Module['webxr_session'];
    if(s) Module['webxr_session'].end();
},

webxr_set_projection_params: function(near, far) {
    var s = Module['webxr_session'];
    if(!s) return;

    /* WEBXR-PORT PATCH #9: direct assignment to session.depthNear/depthFar
     * is the deprecated pre-spec API (silently ignored on current browsers);
     * the WebXR spec routes these through updateRenderState. */
    s.updateRenderState({ depthNear: near, depthFar: far });
},

webxr_set_session_blur_callback: function(callback, userData) {
    WebXR._set_session_callback("blur", callback, userData);
},

webxr_set_session_focus_callback: function(callback, userData) {
    WebXR._set_session_callback("focus", callback, userData);
},

webxr_set_select_callback: function(callback, userData) {
    WebXR._set_input_callback("select", callback, userData);
},
webxr_set_select_start_callback: function(callback, userData) {
    WebXR._set_input_callback("selectstart", callback, userData);
},
webxr_set_select_end_callback: function(callback, userData) {
    WebXR._set_input_callback("selectend", callback, userData);
},

webxr_get_input_sources: function(outArrayPtr, max, outCountPtr) {
    let s = Module['webxr_session'];
    if(!s) return; // TODO(squareys) warning or return error

    let i = 0;
    for (let inputSource of s.inputSources) {
        if(i >= max) break;
        outArrayPtr = WebXR._nativize_input_source(outArrayPtr, inputSource, i);
        ++i;
    }
    setValue(outCountPtr, i, 'i32');
},

/* WEBXR-PORT PATCH #12: full controller snapshot — grip + aim poses and the
 * Gamepad API ("xr-standard") buttons/axes/haptics presence, marshaled into a
 * WebXRControllerState (webxr.h; 196 bytes, layout mirrored here). */
webxr_get_controller_state: function(hand, outPtr) {
    /* zero the whole struct first so absent hands/fields read as 0 */
    for (let w = 0; w < 49; ++w) setValue(outPtr + w*4, 0, 'i32');

    const s = Module['webxr_session'];
    const f = Module['webxr_frame'];
    if (!s) return 0;

    const handStr = hand === 0 ? 'left' : 'right';
    let src = null;
    for (let inputSource of s.inputSources) {
        if (inputSource.handedness === handStr) { src = inputSource; break; }
    }
    if (!src) return 0;

    setValue(outPtr + 0, 1, 'i32'); /* present */

    /* poses need the XRFrame (only valid inside the frame callback) */
    const ref = f ? WebXR.refSpaces[WebXR.refSpace] : null;
    if (f && ref && src.gripSpace) {
        const p = f.getPose(src.gripSpace, ref);
        if (p && !Number.isNaN(p.transform.matrix[0])) {
            setValue(outPtr + 4, 1, 'i32'); /* gripValid */
            WebXR._nativize_vec3(outPtr + 8, p.transform.position);
            WebXR._nativize_vec4(outPtr + 20, p.transform.orientation);
        }
    }
    if (f && ref && src.targetRaySpace) {
        const p = f.getPose(src.targetRaySpace, ref);
        if (p && !Number.isNaN(p.transform.matrix[0])) {
            setValue(outPtr + 36, 1, 'i32'); /* aimValid */
            WebXR._nativize_vec3(outPtr + 40, p.transform.position);
            WebXR._nativize_vec4(outPtr + 52, p.transform.orientation);
        }
    }

    const gp = src.gamepad;
    if (gp) {
        setValue(outPtr + 68, 1, 'i32'); /* gamepadConnected */
        const act = gp.hapticActuators && gp.hapticActuators[0];
        setValue(outPtr + 72, (act && (act.pulse || act.playEffect)) ? 1 : 0, 'i32');
        const nb = Math.min(gp.buttons.length, 8);
        const na = Math.min(gp.axes.length, 4);
        setValue(outPtr + 76, nb, 'i32');
        setValue(outPtr + 80, na, 'i32');
        for (let i = 0; i < nb; ++i) {
            const b = gp.buttons[i];
            setValue(outPtr + 84 + i*12 + 0, b.pressed ? 1 : 0, 'i32');
            setValue(outPtr + 84 + i*12 + 4, b.touched ? 1 : 0, 'i32');
            setValue(outPtr + 84 + i*12 + 8, b.value || 0, 'float');
        }
        for (let i = 0; i < na; ++i)
            setValue(outPtr + 180 + i*4, gp.axes[i] || 0, 'float');
    }
    return 1;
},

/* WEBXR-PORT PATCH #13: haptic pulse with graceful no-op fallback. */
webxr_haptic_pulse: function(hand, intensity, durationMs) {
    const s = Module['webxr_session'];
    if (!s) return 0;
    const handStr = hand === 0 ? 'left' : 'right';
    intensity = Math.min(Math.max(intensity, 0), 1);
    for (let src of s.inputSources) {
        if (src.handedness !== handStr || !src.gamepad) continue;
        const act = src.gamepad.hapticActuators && src.gamepad.hapticActuators[0];
        if (!act) continue;
        try {
            if (act.pulse) { act.pulse(intensity, durationMs); return 1; }
            if (act.playEffect) {
                act.playEffect('dual-rumble', { duration: durationMs,
                    strongMagnitude: intensity, weakMagnitude: intensity });
                return 1;
            }
        } catch(e) { /* actuator rejected the request — treat as no-op */ }
    }
    return 0;
},

webxr_get_input_pose: function(source, outPosePtr, space) {
    let f = Module['webxr_frame'];
    if(!f) {
        console.warn("Cannot call webxr_get_input_pose outside of frame callback");
        return false;
    }

    const id = getValue(source, 'i32');
    const input = Module['webxr_session'].inputSources[id];

    const s = space == 0 ? input.gripSpace : input.targetRaySpace;
    if(!s) return false;
    const pose = f.getPose(s, WebXR.refSpaces[WebXR.refSpace]);

    if(!pose || Number.isNaN(pose.transform.matrix[0])) return false;

    WebXR._nativize_rigid_transform(outPosePtr, pose.transform);

    return true;
},

};

autoAddDeps(LibraryWebXR, '$WebXR');
mergeInto(LibraryManager.library, LibraryWebXR);
