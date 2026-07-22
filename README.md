# QuakeQuest Web — Quake VR in the browser

A WebXR port of [QuakeQuest](https://github.com/DrBeef/QuakeQuest) (Team Beef's
DarkPlaces-based 6DoF Quake VR port for standalone headsets), compiled to
WebAssembly with Emscripten. Plays the shareware episode out of the box; owners
of the full game can drop in their own `pak` files, which are stored
browser-locally (IndexedDB) and never uploaded anywhere.

**Play it:** https://dionysus.dk/webxr/Ports/QuakeQuest/ — works flatscreen in any
browser, or click *Enter VR* in a headset browser (developed and QA'd on
Meta Quest 3). Installable as a PWA with full offline support.

The planned canonical ports layout is
`https://dionysus.dk/webxr/Ports/QuakeQuest/`, with the legacy URL retained as
an internal compatibility alias for existing service-worker and Quest TWA
installations. See [DEPLOYMENT.md](DEPLOYMENT.md); the migration is not deployed
by this source commit.

## Features

- Full 6DoF VR: head tracking, decoupled weapon aim with laser sight,
  stereo rendering straight into the WebXR layer framebuffer
- Smooth locomotion + snap turn, artificial crouch, comfort options,
  recenter, VR menu on a floating quad
- Projection-derived per-eye HUD/text parallax (`vr_hud_depth` cvar) so 2D
  overlays fuse correctly at any eye-buffer resolution
- Persistent saves/config via IndexedDB (`-userdir /quake_user`)
- Launcher choice between bundled legal shareware and a browser-local import
  from an owned Quake/id1 folder, followed by flatscreen/WASM or WebXR mode
- PWA: installable, offline-capable, versioned service-worker cache with
  automatic single-reload updates
- ~25x faster than a naive port: real VBO path (`gl_webgl_forcevbo`) instead
  of Emscripten's `FULL_ES2` client-array emulation

## VR controls (right-handed default)

| Input | Action |
|---|---|
| Right trigger | fire |
| Left trigger | run |
| Right B / A | jump / duck (hold) |
| Left X | menu open/close |
| Left stick | move |
| Right stick | snap turn; flick up/down = next/previous weapon |
| Right stick click | laser sight |
| Left stick click | menu (short) / recenter (hold) |

No controller button ever quick-saves or quick-loads; save/load lives in the
menu (F6/F9 still work flatscreen).

## Layout

- `darkplaces/` — the QuakeQuest engine fork; all port edits are marked with
  `WEBXR-PORT` comments and inventoried in [PORT_NOTES.md](PORT_NOTES.md)
- `web-host/` — new browser host layer (replaces the Android/OpenXR host):
  main loop, WebXR bridge, input/locomotion/weapon/menu/comfort modules, a
  vendored & patched [emscripten-webxr](web-host/lib/webxr/PATCHES.md)
  library, and headless test suites (`web-host/test/*.mjs`, puppeteer +
  [IWER](https://github.com/meta-quest/immersive-web-emulation-runtime)
  emulated XR)
- `web-page/` — the authored page shell: `index.html`, PWA manifest, service
  worker, icons
- `build.sh` — Emscripten build; outputs `quake.js/.wasm/.data` plus the
  stamped page shell

## Building

Requires [emsdk](https://github.com/emscripten-core/emsdk) and game data at
`../data/id1/pak0.pak` (shareware v1.06). Then:

```sh
./build.sh            # output in ../web
node ../web/serve.mjs 8090   # local server with COOP/COEP headers
```

The COOP/COEP (cross-origin isolation) headers are required in production
too — see `web-page/sw.js` and the Apache snippets in PORT_NOTES.md.

## Testing

Headless suites under `web-host/test/` cover input, locomotion, weapons, HUD
parallax (per-eye pixel measurement), comfort, session lifecycle
(enter/exit/re-enter across three simulated UA behaviors), game-data drop-in,
PWA install/offline/update, and an all-up integration pass:

```sh
node web-host/test/m3-integration-test.mjs   # expects the 8090 server
```

## License

GPL-2.0-or-later — see [LICENSE.md](LICENSE.md) and `darkplaces/COPYING`.
Quake and the shareware data are © id Software; the shareware episode is
redistributable per its original terms and the full game data is not included.
Thanks to Team Beef / DrBeef for QuakeQuest and to LadyHavoc et al. for
DarkPlaces.
