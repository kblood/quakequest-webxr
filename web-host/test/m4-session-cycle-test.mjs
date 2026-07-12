#!/usr/bin/env node
/*
 * m4-session-cycle-test.mjs — VR enter -> exit -> re-enter lifecycle checks
 * (real-headset QA bug 3: after leaving VR the user could neither re-enter
 * VR nor get the pointer-lock-exit -> menu synthesis to work again; both
 * symptoms point at stale session-active state after exit).
 *
 * Runs the full cycle in TWO harness modes:
 *   - "canvas" — plain IWER: XRWebGLLayer.framebuffer === null, rendering
 *     goes to the canvas backbuffer (what all pre-existing suites test);
 *   - "layerfb" — DEVICE MODE: the layer exposes a real WebGLFramebuffer
 *     like an actual Quest browser. This exercises the library's
 *     glLayer.framebuffer branch (GL.framebuffers registration,
 *     Module.webxr_fbo, PATCH #3 end-cleanup) and the bridge/menuquad
 *     layer-FBO binds — none of which run under plain IWER.
 *
 * Cycle per mode:
 *   1. flatscreen pointer-lock exit synthesizes Esc -> menu opens (baseline);
 *   2. enter VR -> both eyes render distinct content (into the mode's target);
 *   3. exit VR -> C-side WebXRBridge_IsSessionActive() reads FALSE, page got
 *      onQuakeXRState(false), flatscreen loop resumed (frames advance);
 *   4. pointer-lock exit STILL synthesizes Esc (guard reads the flag);
 *   5. re-enter VR -> both eyes render again;
 *   6. exit again (cycle repeatable) + pointer-lock Esc again.
 *
 * Prereqs: build served on 8090 (node web/serve.mjs 8090 from webxr-port/).
 * Env overrides: see m3-input-test.mjs header.
 * Usage: node m4-session-cycle-test.mjs [screenshot-prefix]
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
const SHOT = process.argv[2] || 'm4-session-cycle';

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

/* device-mode shim: give IWER's XRWebGLLayer a REAL framebuffer, like the
 * Quest browser. Dimensions are frozen at first access (real layers size
 * their buffers at creation; IWER's getters track the live canvas, which
 * the engine resizes mid-session). */
const LAYER_FB_SHIM = `
;(function(){
  const IwerLayer = window.XRWebGLLayer;
  window.XRWebGLLayer = class extends IwerLayer {
    constructor(session, ctx, init) { super(session, ctx, init); this.__ctx = ctx; }
    get framebufferWidth()  { this.__ensure(); return this.__w; }
    get framebufferHeight() { this.__ensure(); return this.__h; }
    get framebuffer()       { this.__ensure(); return this.__fb; }
    __ensure() {
      if (this.__fb) return;
      const gl = this.__ctx;
      this.__w = super.framebufferWidth;
      this.__h = super.framebufferHeight;
      const prevTex = gl.getParameter(gl.TEXTURE_BINDING_2D);
      const prevRb  = gl.getParameter(gl.RENDERBUFFER_BINDING);
      const prevFb  = gl.getParameter(gl.FRAMEBUFFER_BINDING);
      const fb = gl.createFramebuffer();
      const tex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, this.__w, this.__h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
      const rb = gl.createRenderbuffer();
      gl.bindRenderbuffer(gl.RENDERBUFFER, rb);
      gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH24_STENCIL8, this.__w, this.__h);
      gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
      gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, rb);
      gl.bindTexture(gl.TEXTURE_2D, prevTex);
      gl.bindRenderbuffer(gl.RENDERBUFFER, prevRb);
      gl.bindFramebuffer(gl.FRAMEBUFFER, prevFb);
      this.__fb = fb;
      window.__xrLayerInfo = { fb, w: this.__w, h: this.__h };
    }
  };
})();`;

/* end-quirk shim: emulate a UA where XRSession.cancelAnimationFrame /
 * requestAnimationFrame THROW InvalidStateError once the session has ended
 * (the ORIGINAL WebXR spec behavior, kept by older Chromium builds). The
 * vendored library's 'end' listener starts with a cancelAnimationFrame call
 * on the already-ended session — on such UAs an exception there kills the
 * whole teardown chain before the C-side end callback runs. */
const END_QUIRK_SHIM = `
;(function(){
  const rafOrig = XRSession.prototype.requestAnimationFrame;
  const cafOrig = XRSession.prototype.cancelAnimationFrame;
  let ended = new WeakSet();
  const endOrig = XRSession.prototype.end;
  XRSession.prototype.end = function() { ended.add(this); return endOrig.call(this); };
  XRSession.prototype.requestAnimationFrame = function(cb) {
    if (ended.has(this)) throw new DOMException('session ended', 'InvalidStateError');
    return rafOrig.call(this, cb);
  };
  XRSession.prototype.cancelAnimationFrame = function(h) {
    if (ended.has(this)) throw new DOMException('session ended', 'InvalidStateError');
    return cafOrig.call(this, h);
  };
})();`;

async function runMode(mode) {
  console.log(`\n== mode: ${mode} ==`);
  const page = await browser.newPage();
  page.setDefaultTimeout(45000);

  const errors = [];
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push('PAGEERROR: ' + e.message));

  await page.evaluateOnNewDocument(iwerSrc + `
;(function(){
  const dev = new IWER.XRDevice(IWER.metaQuest3);
  dev.installRuntime({ forceInstall: true });
  dev.stereoEnabled = true;
  window.__xrdevice = dev;
  /* record every onQuakeXRState transition the page receives */
  window.__xrStates = [];
  let cur;
  Object.defineProperty(window, 'onQuakeXRState', {
    get() { return cur; },
    set(fn) { cur = (a) => { window.__xrStates.push(!!a); return fn(a); }; },
    configurable: true,
  });
})();` + (mode === 'layerfb' ? LAYER_FB_SHIM : '')
      + (mode === 'endquirk' ? END_QUIRK_SHIM : ''));

  const check = (name, cond, detail) => {
    console.log(`  [${cond ? 'PASS' : 'FAIL'}] ${mode}/${name}${detail !== undefined ? ' — ' + detail : ''}`);
    if (!cond) fails.push(`${mode}/${name}`);
  };

  const menuSt = async () => {
    const v = await page.evaluate(() => Module._VRMenuQuad_DebugState());
    return { raw: v, quad: v & 1, bigScreen: (v >> 4) & 0xf,
             keyDest: (v >> 12) & 0xf, mState: (v >> 16) & 0xff };
  };
  const sessionActive = () => page.evaluate(() =>
    Module._WebXRBridge_IsSessionActive ? !!Module._WebXRBridge_IsSessionActive() : null);

  /* per-eye render probe: 3x3 grid per eye viewport, read from the layer
   * framebuffer (device mode) or the canvas backbuffer (plain IWER) */
  const probeEyes = () => page.evaluate(() => new Promise((resolve, reject) => {
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
        const out = pose.views.map((view) => {
          const vp = layer.getViewport(view);
          let lit = 0, sum = 0;
          for (let iy = 1; iy <= 3; iy++)
            for (let ix = 1; ix <= 3; ix++) {
              const b = new Uint8Array(4);
              gl.readPixels((vp.x + vp.width * ix / 4) | 0,
                            (vp.y + vp.height * iy / 4) | 0,
                            1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
              const l = Math.max(b[0], b[1], b[2]);
              if (l > 8) lit++;
              sum += b[0] + b[1] + b[2];
            }
          return { eye: view.eye, lit, sum };
        });
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, prevRead);
        resolve(out);
      });
    }, reject);
  }));

  const pointerLockEscCheck = async (tag) => {
    await page.click('#canvas');
    await sleep(400);
    const locked = await page.evaluate(() => document.pointerLockElement !== null);
    check(`${tag}: canvas click acquires pointer lock`, locked);
    let st = await menuSt();
    check(`${tag}: in-game before unlock (m_state=m_none)`, st.mState === 0, JSON.stringify(st));
    await page.evaluate(() => document.exitPointerLock());
    await sleep(500);
    st = await menuSt();
    check(`${tag}: pointer-lock exit synthesized Esc -> menu open`,
      st.mState === 1, JSON.stringify(st));
    await page.keyboard.press('Escape');
    await sleep(400);
    st = await menuSt();
    check(`${tag}: menu closed again`, st.mState === 0, JSON.stringify(st));
  };

  const enterVR = async (tag) => {
    await page.waitForFunction(() => !document.getElementById('entervr')?.disabled);
    await page.click('#entervr');
    await page.waitForFunction(
      () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 20000 });
    await sleep(1200);
    check(`${tag}: C-side session flag TRUE in VR`, await sessionActive() === true);
    const eyes = await probeEyes();
    console.log('  eyes:', JSON.stringify(eyes));
    check(`${tag}: both eyes render`, eyes.length === 2 && eyes.every((e) => e.lit >= 5),
      JSON.stringify(eyes));
    check(`${tag}: eyes differ (stereo, not a copy)`, eyes[0].sum !== eyes[1].sum,
      `${eyes[0].sum} vs ${eyes[1].sum}`);
  };

  const exitVR = async (tag) => {
    await page.evaluate(() => document.getElementById('entervr').click());
    await page.waitForFunction(
      () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 20000 });
    await sleep(800);
    check(`${tag}: C-side session flag FALSE after exit`, await sessionActive() === false);
    const states = await page.evaluate(() => window.__xrStates.slice());
    check(`${tag}: page saw onQuakeXRState(false)`, states[states.length - 1] === false,
      JSON.stringify(states));
    /* flatscreen loop resumed: host frame counter advances */
    const f1 = await page.evaluate(() => Module._WebHost_FrameCount ? Module._WebHost_FrameCount() : -1);
    await sleep(400);
    const f2 = await page.evaluate(() => Module._WebHost_FrameCount ? Module._WebHost_FrameCount() : -1);
    check(`${tag}: flatscreen loop resumed (frames advance)`, f1 === -1 ? false : f2 > f1,
      `${f1} -> ${f2}`);
  };

  console.log('# loading ' + URL);
  await page.goto(URL, { waitUntil: 'load' });
  await page.waitForFunction(
    () => document.getElementById('start')?.textContent === 'Running', { timeout: 45000 });
  await page.waitForFunction(
    () => Module._IN_Weapon_Probe && Module._IN_Weapon_Probe(15) === 4 && Module._IN_Weapon_Probe(14) > 0,
    { timeout: 45000 });
  console.log('# map start up');

  await pointerLockEscCheck('pre-VR');
  await enterVR('enter#1');
  await page.screenshot({ path: `${SHOT}-${mode}-vr1.png` });
  await exitVR('exit#1');
  await pointerLockEscCheck('post-VR');
  await enterVR('enter#2');
  await page.screenshot({ path: `${SHOT}-${mode}-vr2.png` });
  await exitVR('exit#2');
  await pointerLockEscCheck('post-VR#2');

  const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
  console.log(`# ${mode} console errors: ` + realErrors.length);
  realErrors.forEach((e) => console.log('  ERR: ' + e));
  if (realErrors.length) fails.push(...realErrors.map((e) => `${mode}/console: ${e}`));

  await page.close();
}

const MODES = (process.env.QQ_MODES || 'canvas,layerfb,endquirk').split(',');
for (const m of MODES) {
  try {
    await runMode(m);
  } catch (e) {
    console.log(`  [FAIL] ${m}/harness: ${e.message.split('\n')[0]}`);
    fails.push(`${m}/harness: ${e.message.split('\n')[0]}`);
  }
}

await browser.close();
const ok = fails.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + fails.join('; ') + ')'));
process.exit(ok ? 0 : 1);
