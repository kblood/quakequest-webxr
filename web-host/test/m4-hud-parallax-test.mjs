#!/usr/bin/env node
/*
 * m4-hud-parallax-test.mjs — stereo 2D overlay disparity measurement
 * (real-headset QA bug 2: in-game message text doubles between the eyes).
 *
 * The fork gave every 2D overlay element a different hardcoded per-eye
 * x-offset (centerprint ±10, sbar ±20, crosshair ±5, console notify NONE),
 * none of which compensated the HMD's asymmetric per-eye frusta. The fix
 * routes them all through VR_Stereo2DOffset() (gl_rmain.c), which derives a
 * single con-unit offset per eye from the cached XR projection matrices and
 * the vr_hud_depth cvar (meters):
 *     ndc_e = sign_e * P0 * IPD/(2*depth) - P8   (P0=proj[0], P8=proj[8])
 *
 * Phase "sym" (plain IWER, symmetric frusta — P8 = 0):
 *   1. boot +map start, enter VR (both eyes side-by-side on the canvas
 *      backbuffer), black out the 3D view (v_cshift 0 0 0 255), inject
 *      centerprint (WebHost_CenterPrint) + notify text (echo); sbar is
 *      already drawn;
 *   2. readPixels per-eye horizontal bands, build per-column profiles,
 *      cross-correlate left vs right -> measured pixel disparity;
 *   3. compare against the prediction from the very projections IWER handed
 *      the engine (Module._WebXRBridge_2DParallaxNDC):
 *        expected_px = (ndcL - ndcR) * eyeWidth / 2   (conwidth cancels);
 *   4. re-check at vr_hud_depth 0.75 (offset must track the cvar) and at
 *      vr_hud_depth 0 (legacy escape hatch -> zero disparity);
 *   5. assert crossed direction + all elements at ONE depth.
 *
 * Phase "asym" (XRView.projectionMatrix shimmed to P8 = -/+0.15, i.e. the
 * outward-canted frusta of a real HMD — the actual on-device failure mode):
 *   same measurement; expected disparity now dominated by the P8 term
 *   (~73 px instead of ~13 px at the same depth), proving the engine picks
 *   the asymmetry up from the live projection matrices, and that the
 *   left-anchored notify text stays fully on screen (VR_Stereo2DOffsetBase).
 *
 * Prereqs: build served on 8090 (node web/serve.mjs 8090 from webxr-port/).
 * Usage: node m4-hud-parallax-test.mjs [screenshot-prefix]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
const URL = process.env.QQ_URL || 'http://localhost:8090/?autostart=1&args='
  + encodeURIComponent('+skill 0 +map start');
const SHOT = process.argv[2] || 'm4-hud-parallax';

const CHROME = [process.env.QQ_CHROME,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']
  .filter(Boolean).find(existsSync);
if (!CHROME) { console.error('no chrome found (set QQ_CHROME)'); process.exit(2); }
if (!existsSync(IWER_PATH)) { console.error('iwer not found (set QQ_IWER_JS): ' + IWER_PATH); process.exit(2); }

const iwerSrc = readFileSync(IWER_PATH, 'utf8');
const browser = await puppeteer.launch({
  executablePath: CHROME, headless: true,
  args: ['--no-sandbox', '--enable-features=SharedArrayBuffer'],
});

const fails = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* asymmetric-frustum shim: rewrite each XRView's projectionMatrix m[8]
 * (the horizontal frustum-asymmetry term) to what an outward-canted HMD
 * reports: negative for the left eye, positive for the right. Handles both
 * prototype-getter and own-property implementations; always returns a
 * fresh copy so repeated reads can't double-apply. */
const ASYM_P8 = 0.15;
const ASYM_SHIM = `
;(function(){
  const P8 = ${ASYM_P8};
  function patchedMatrix(view, m) {
    const c = new Float32Array(m);
    c[8] = (view.eye === 'left' ? -P8 : (view.eye === 'right' ? P8 : 0));
    return c;
  }
  const patchedProtos = new WeakSet();
  const gvp = XRFrame.prototype.getViewerPose;
  XRFrame.prototype.getViewerPose = function(ref) {
    const pose = gvp.call(this, ref);
    if (pose) for (const v of pose.views) {
      const proto = Object.getPrototypeOf(v);
      const d = Object.getOwnPropertyDescriptor(proto, 'projectionMatrix');
      if (d && d.get) {
        if (!patchedProtos.has(proto)) {
          patchedProtos.add(proto);
          Object.defineProperty(proto, 'projectionMatrix', {
            get() { return patchedMatrix(this, d.get.call(this)); },
            configurable: true,
          });
        }
      } else {
        // own data property: overwrite in place (absolute set = idempotent)
        v.projectionMatrix = patchedMatrix(v, v.projectionMatrix);
      }
    }
    return pose;
  };
})();`;

/* bands are fractions of viewport height from the TOP (readPixels is
 * bottom-up; converted inside). "bright" = white-ish text pixels; "lum" =
 * mean luminance (for the multicolored sbar). The fork places the sbar 40
 * con-units above the bottom edge: sbar_y = 480 - 40 - 24 -> rows 416..440. */
const BANDS = {
  notify:      { y0: 0.00, y1: 0.10, x0: 0.00, x1: 0.60, kind: 'bright' },
  centerprint: { y0: 0.45, y1: 0.62, x0: 0.10, x1: 0.90, kind: 'bright' },
  sbar:        { y0: 0.85, y1: 0.93, x0: 0.10, x1: 0.90, kind: 'lum' },
};

/* integer cross-correlation + parabolic refinement: returns shift s (px)
 * such that LEFT content sits s px to the RIGHT of the same RIGHT-eye
 * content (positive = crossed disparity = fuses in front of infinity) */
function disparity(profL, profR, maxShift = 100) {
  const n = Math.min(profL.length, profR.length);
  const mean = (p) => p.reduce((a, v) => a + v, 0) / p.length;
  const mL = mean(profL), mR = mean(profR);
  const L = profL.map((v) => v - mL), R = profR.map((v) => v - mR);
  let best = 0, bestScore = -Infinity;
  const scores = new Map();
  for (let s = -maxShift; s <= maxShift; s++) {
    let acc = 0;
    for (let i = Math.max(0, -s); i < Math.min(n, n - s); i++) acc += L[i + s] * R[i];
    scores.set(s, acc);
    if (acc > bestScore) { bestScore = acc; best = s; }
  }
  const y0 = scores.get(best - 1), y1 = scores.get(best), y2 = scores.get(best + 1);
  let sub = best;
  if (y0 !== undefined && y2 !== undefined) {
    const den = y0 - 2 * y1 + y2;
    if (den !== 0) sub = best + 0.5 * (y0 - y2) / den;
  }
  return sub;
}

async function runPhase(phase) {
  console.log(`\n== phase: ${phase} ==`);
  const page = await browser.newPage();
  page.setDefaultTimeout(45000);
  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push('PAGEERROR: ' + e.message));

  const check = (name, cond, detail) => {
    console.log(`  [${cond ? 'PASS' : 'FAIL'}] ${phase}/${name}${detail !== undefined ? ' — ' + detail : ''}`);
    if (!cond) fails.push(`${phase}/${name}`);
  };

  await page.evaluateOnNewDocument(iwerSrc + `
;(function(){
  const dev = new IWER.XRDevice(IWER.metaQuest3);
  dev.installRuntime({ forceInstall: true });
  dev.stereoEnabled = true;
  window.__xrdevice = dev;
})();` + (phase === 'asym' ? ASYM_SHIM : ''));

  console.log('# loading ' + URL);
  await page.goto(URL, { waitUntil: 'load' });
  await page.waitForFunction(
    () => document.getElementById('start')?.textContent === 'Running', { timeout: 45000 });
  await page.waitForFunction(
    () => Module._IN_Weapon_Probe && Module._IN_Weapon_Probe(15) === 4 && Module._IN_Weapon_Probe(14) > 0,
    { timeout: 45000 });
  console.log('# map start up');

  await page.waitForFunction(() => !document.getElementById('entervr')?.disabled);
  await page.click('#entervr');
  await page.waitForFunction(
    () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 20000 });
  await sleep(1500);

  const exec = (cmd) => page.evaluate((c) =>
    Module.ccall('WebHost_Exec', null, ['string'], [c]), cmd);
  const centerprint = (msg) => page.evaluate((m) =>
    Module.ccall('WebHost_CenterPrint', null, ['string'], [m]), msg);

  check('C-side session active', await page.evaluate(() => !!Module._WebXRBridge_IsSessionActive()));

  await exec('v_cshift 0 0 0 255');       // solid black under the 2D stage
  await exec('scr_centertime 9999');
  await exec('con_notifytime 9999');
  await exec('crosshair 0');              // keep the centerprint band clean
  await exec('vr_hud_depth 1.5');         // the default, but pin it for the math

  /* centerprint reveal/erase timing can retire the text between
   * measurements — stage it fresh before every grab */
  const stageText = async () => {
    await centerprint('MMMMMMMMMMMMMMMM\nMMMMMMMMMMMMMMMM\nMMMMMMMMMMMMMMMM');
    await exec('echo ^7MMMMMMMMMMMMMMMMMMMMMMMM');
    await exec('echo ^7MMMMMMMMMMMMMMMMMMMMMMMM');
    await sleep(900);
  };

  /* per-eye band profiles straight off the eye buffers */
  const grab = () => page.evaluate((bands) => new Promise((resolve, reject) => {
    const session = Module['webxr_session'];
    if (!session) { reject(new Error('no session')); return; }
    session.requestReferenceSpace('viewer').then((refSpace) => {
      session.requestAnimationFrame((t, frame) => {
        const gl = document.getElementById('canvas').getContext('webgl2');
        const layer = session.renderState.baseLayer;
        const pose = frame.getViewerPose(refSpace);
        if (!pose) { reject(new Error('no pose')); return; }
        const prevRead = gl.getParameter(gl.READ_FRAMEBUFFER_BINDING);
        if (layer.framebuffer)
          gl.bindFramebuffer(gl.READ_FRAMEBUFFER, layer.framebuffer);
        const out = {};
        for (const view of pose.views) {
          const vp = layer.getViewport(view);
          const eyeOut = { vp: { x: vp.x, y: vp.y, w: vp.width, h: vp.height }, bands: {} };
          for (const [name, b] of Object.entries(bands)) {
            const rowTop = Math.floor(b.y0 * vp.height), rowBot = Math.ceil(b.y1 * vp.height);
            const h = rowBot - rowTop;
            const glY = vp.y + vp.height - rowBot; // top-origin rows -> GL bottom-origin y
            const buf = new Uint8Array(vp.width * h * 4);
            gl.readPixels(vp.x, glY, vp.width, h, gl.RGBA, gl.UNSIGNED_BYTE, buf);
            const c0 = Math.floor(b.x0 * vp.width), c1 = Math.ceil(b.x1 * vp.width);
            const prof = new Array(vp.width).fill(0);
            for (let yy = 0; yy < h; yy++)
              for (let xx = c0; xx < c1; xx++) {
                const i = (yy * vp.width + xx) * 4;
                const r = buf[i], g = buf[i + 1], bl = buf[i + 2];
                if (b.kind === 'bright') prof[xx] += (r > 170 && g > 170 && bl > 170) ? 1 : 0;
                else prof[xx] += (r + g + bl) / 3;
              }
            eyeOut.bands[name] = prof;
          }
          out[view.eye] = eyeOut;
        }
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, prevRead);
        resolve(out);
      });
    }, reject);
  }), BANDS);

  const measure = async (tag, depth) => {
    const shot = await grab();
    const left = shot.left, right = shot.right;
    if (!left || !right) throw new Error('missing eye views: ' + Object.keys(shot));
    const eyeW = left.vp.w;
    const [ndcL, ndcR] = await page.evaluate((d) => [
      Module._WebXRBridge_2DParallaxNDC(0, d),
      Module._WebXRBridge_2DParallaxNDC(1, d)], depth);
    const expectPx = (ndcL - ndcR) * eyeW / 2;
    console.log(`# ${tag}: eyeW=${eyeW} ndcL=${ndcL.toFixed(5)} ndcR=${ndcR.toFixed(5)} expected disparity=${expectPx.toFixed(1)}px`);
    check(`${tag}: crossed direction from projections (ndcL>0>ndcR)`, ndcL > 0 && ndcR < 0,
      `${ndcL.toFixed(5)} / ${ndcR.toFixed(5)}`);
    const res = {};
    for (const name of Object.keys(BANDS)) {
      const hasSignal = left.bands[name].some((v) => v > 0) && right.bands[name].some((v) => v > 0);
      check(`${tag}/${name}: content visible in both eyes`, hasSignal);
      if (!hasSignal) continue;
      const d = disparity(left.bands[name], right.bands[name]);
      res[name] = d;
      const tol = Math.max(3, Math.abs(expectPx) * 0.25);
      check(`${tag}/${name}: measured disparity matches projection math`,
        Math.abs(d - expectPx) <= tol,
        `measured ${d.toFixed(1)}px vs expected ${expectPx.toFixed(1)}px (tol ±${tol.toFixed(1)})`);
    }
    const vals = Object.values(res);
    if (vals.length >= 2) {
      const spread = Math.max(...vals) - Math.min(...vals);
      check(`${tag}: all elements at ONE depth (spread <= 3px)`, spread <= 3,
        Object.entries(res).map(([k, v]) => `${k}=${v.toFixed(1)}`).join(' ') + ` (spread ${spread.toFixed(1)})`);
    }
    return { ndcL, ndcR, expectPx, res };
  };

  await stageText();
  await page.screenshot({ path: `${SHOT}-${phase}.png` });
  const m1 = await measure('depth1.5', 1.5);

  if (phase === 'asym') {
    /* the whole point of this phase: the engine's cached projections must
     * carry the shimmed P8 and the offset must be dominated by it */
    check('cached projections carry the frustum asymmetry',
      Math.abs(m1.ndcL - (0.0325 + ASYM_P8)) < 0.02 && Math.abs(m1.ndcR + (0.0325 + ASYM_P8)) < 0.02,
      `ndcL=${m1.ndcL.toFixed(4)} (want ~${(0.0325 + ASYM_P8).toFixed(4)})`);
  } else {
    /* cvar-driven: nearer depth must increase the crossed disparity */
    await exec('vr_hud_depth 0.75');
    await stageText();
    const m2 = await measure('depth0.75', 0.75);
    check('disparity tracks vr_hud_depth (0.75m > 1.5m offset)',
      m2.expectPx > m1.expectPx + 1 && (m2.res.centerprint ?? 0) > (m1.res.centerprint ?? 99),
      `centerprint ${m1.res.centerprint?.toFixed(1)}px @1.5m -> ${m2.res.centerprint?.toFixed(1)}px @0.75m`);

    /* legacy escape hatch: vr_hud_depth 0 disables all offsets */
    await exec('vr_hud_depth 0');
    await stageText();
    const shot0 = await grab();
    const sig0 = shot0.left.bands.centerprint.some((v) => v > 0)
      && shot0.right.bands.centerprint.some((v) => v > 0);
    check('depth0: centerprint visible in both eyes', sig0);
    if (sig0) {
      const d0 = disparity(shot0.left.bands.centerprint, shot0.right.bands.centerprint);
      check('vr_hud_depth 0 -> zero buffer disparity', Math.abs(d0) <= 2, `${d0.toFixed(1)}px`);
    }
  }

  const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D'));
  console.log(`# ${phase} console errors: ` + realErrors.length);
  realErrors.forEach((e) => console.log('  ERR: ' + e));
  if (realErrors.length) fails.push(...realErrors.map((e) => `${phase}/console: ` + e));

  await page.close();
}

for (const phase of (process.env.QQ_PHASES || 'sym,asym').split(',')) {
  try {
    await runPhase(phase);
  } catch (e) {
    console.log(`  [FAIL] ${phase}/harness: ${e.message.split('\n')[0]}`);
    fails.push(`${phase}/harness: ${e.message.split('\n')[0]}`);
  }
}

await browser.close();
const ok = fails.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + fails.join('; ') + ')'));
process.exit(ok ? 0 : 1);
