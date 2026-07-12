#!/usr/bin/env node
/*
 * m3-hud-test.mjs — emulated-XR verification of M3 chunk 3 (HUD & menus):
 *  - 2D-UI mode (demo/menu/console) renders as a WORLD-ANCHORED quad inside
 *    the scene, not a head-locked full-view (pixel probe: quad occupies the
 *    view center with black void at the eye corners; rotating the emulated
 *    head moves the quad OUT of the view center);
 *  - menu toggle rebind: OFF-HAND (left, default cl_righthanded 1)
 *    thumbstick click opens/closes the menu;
 *  - menu navigation: emulated d-pad (thumbstick edges) + A=enter/B=back
 *    change the engine's menu state (asserted via the C-side probe
 *    Module._VRMenuQuad_DebugState);
 *  - in-game (no 2D UI) the normal stereo path stays active.
 *
 * Same harness pattern as m3-input-test.mjs (IWER emulated Quest 3 in
 * headless Chrome). Serve first: node webdist/serve.mjs 8094
 *
 * Usage: node m3-hud-test.mjs [screenshot-prefix]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
const URL = process.env.QQ_URL || 'http://localhost:8094/?autostart=1';
const SHOT = process.argv[2] || 'm3-hud';

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
const page = await browser.newPage();
page.setDefaultTimeout(30000);

const errors = [];
page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
page.on('pageerror', (e) => errors.push('PAGEERROR: ' + e.message));

await page.evaluateOnNewDocument(iwerSrc + `
;(function(){
  const dev = new IWER.XRDevice(IWER.metaQuest3);
  dev.installRuntime({ forceInstall: true });
  dev.stereoEnabled = true;
  window.__xrdevice = dev;
})();`);

const fails = [];
const check = (name, cond, detail) => {
  console.log(`  [${cond ? 'PASS' : 'FAIL'}] ${name}${detail ? ' — ' + detail : ''}`);
  if (!cond) fails.push(name);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* C-side probe: bit0 quad-active, bits4-7 bigScreen, bit8 console,
 * bit9 demoplayback, bits12-15 key_dest, bits16-23 m_state */
const probe = async () => {
  const v = await page.evaluate(() => Module._VRMenuQuad_DebugState());
  return {
    raw: v, quad: v & 1, bigScreen: (v >> 4) & 0xf, console: (v >> 8) & 1,
    demo: (v >> 9) & 1, keyDest: (v >> 12) & 0xf, mState: (v >> 16) & 0xff,
  };
};

/* per-eye pixel probe inside an XR rAF registered after the library's:
 * center + 4 inset corners of each eye viewport, read from the canvas
 * backbuffer (IWER's layer has framebuffer null) */
const probeEyes = () => page.evaluate(() => new Promise((resolve, reject) => {
  const session = Module['webxr_session'];
  if (!session) { reject(new Error('no session')); return; }
  session.requestReferenceSpace('viewer').then((refSpace) => {
    session.requestAnimationFrame((t, frame) => {
      const gl = document.getElementById('canvas').getContext('webgl2');
      const layer = session.renderState.baseLayer;
      const pose = frame.getViewerPose(refSpace);
      if (!pose) { reject(new Error('no pose')); return; }
      const lum = (x, y) => {
        const b = new Uint8Array(4);
        gl.readPixels(x | 0, y | 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, b);
        return Math.max(b[0], b[1], b[2]);
      };
      resolve(pose.views.map((view) => {
        const vp = layer.getViewport(view);
        return {
          eye: view.eye, vp: [vp.x, vp.y, vp.width, vp.height],
          center: lum(vp.x + vp.width / 2, vp.y + vp.height / 2),
          corners: [
            lum(vp.x + 8, vp.y + 8),
            lum(vp.x + vp.width - 8, vp.y + 8),
            lum(vp.x + 8, vp.y + vp.height - 8),
            lum(vp.x + vp.width - 8, vp.y + vp.height - 8),
          ],
        };
      }));
    });
  }, reject);
}));

const leftStickClick = async () => { /* off-hand (default right-handed) = menu toggle */
  await page.evaluate(() => window.__xrdevice.controllers.left.updateButtonValue('thumbstick', 1.0));
  await sleep(250);
  await page.evaluate(() => window.__xrdevice.controllers.left.updateButtonValue('thumbstick', 0.0));
  await sleep(400);
};
const stick = async (hand, x, y) => { /* xr-standard axes (y = +down) */
  await page.evaluate(([h, a, b]) =>
    window.__xrdevice.controllers[h].updateAxes('thumbstick', a, b), [hand, x, y]);
  await sleep(300);
};
const button = async (hand, name, v) => {
  await page.evaluate(([h, n, val]) =>
    window.__xrdevice.controllers[h].updateButtonValue(n, val), [hand, name, v]);
  await sleep(300);
};

console.log('# loading ' + URL);
await page.goto(URL, { waitUntil: 'load' });
await page.waitForFunction(
  () => document.getElementById('start')?.textContent === 'Running', { timeout: 30000 });
console.log('# engine running; letting demo spin up…');
await sleep(9000);

await page.waitForFunction(
  () => !document.getElementById('entervr')?.disabled, { timeout: 15000 });
await page.click('#entervr');
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 15000 });
console.log('# immersive session active');
await sleep(2000);

/* ---- 1) demo playback = 2D-UI mode -> world-anchored quad ---- */
let st = await probe();
console.log('  probe:', JSON.stringify(st));
check('screen-layer quad path active during demo', st.quad === 1 && st.demo === 1, JSON.stringify(st));

let eyes = await probeEyes();
console.log('  eyes:', JSON.stringify(eyes));
check('both eyes probed', eyes.length === 2);
for (const e of eyes) {
  check(`${e.eye}: quad content at view center`, e.center > 16, 'lum=' + e.center);
  check(`${e.eye}: black void at eye corners (quad IS bounded, not full-view)`,
    e.corners.every((c) => c <= 8), 'corners=' + JSON.stringify(e.corners));
}
await page.screenshot({ path: SHOT + '-quad-demo.png' });

/* ---- 2) world-anchored, not head-locked: rotate head 70° right ---- */
await page.evaluate(() => {
  const dev = window.__xrdevice;
  const half = (-70 * Math.PI / 180) / 2; /* yaw right = negative about +Y */
  dev.quaternion.set(0, Math.sin(half), 0, Math.cos(half));
});
await sleep(600);
eyes = await probeEyes();
console.log('  eyes (head rotated 70°):', JSON.stringify(eyes));
for (const e of eyes) {
  check(`${e.eye}: quad left the view center after head turn (world-anchored)`,
    e.center <= 8, 'lum=' + e.center);
}
await page.screenshot({ path: SHOT + '-quad-headturn.png' });
await page.evaluate(() => window.__xrdevice.quaternion.set(0, 0, 0, 1));
await sleep(400);

/* ---- 3) menu toggle binding: off-hand (left) thumbstick click ---- */
await leftStickClick();
st = await probe();
console.log('  probe (after toggle):', JSON.stringify(st));
check('menu opened by off-hand stick click (m_state=m_main)', st.mState === 1, JSON.stringify(st));
check('bigScreen set by engine on menu open', st.bigScreen === 1);
await page.screenshot({ path: SHOT + '-menu-open.png' });

/* ---- 4) d-pad navigation + A(enter) / B(back) ---- */
/* stick down = xr-standard y +0.8 -> K_DOWNARROW: main-menu cursor 0->1
 * (Single Player -> Multiplayer); A -> K_ENTER -> m_multiplayer (=8) */
await stick('left', 0, 0.8);
await stick('left', 0, 0);
await button('right', 'a-button', 1.0);
await button('right', 'a-button', 0.0);
st = await probe();
console.log('  probe (down+enter):', JSON.stringify(st));
check('d-pad down + A entered the 2nd menu item (m_state=m_multiplayer)',
  st.mState === 8, 'mState=' + st.mState);

await button('right', 'b-button', 1.0);
await button('right', 'b-button', 0.0);
st = await probe();
check('B backs out to main menu (m_state=m_main)', st.mState === 1, 'mState=' + st.mState);

/* ---- 5) toggle closes the menu again ---- */
await leftStickClick();
st = await probe();
console.log('  probe (after close):', JSON.stringify(st));
check('menu closed by second stick click (m_state=m_none, bigScreen=0)',
  st.mState === 0 && st.bigScreen === 0, JSON.stringify(st));

/* ---- 6) start a real game via keyboard console; in-game = stereo path ---- */
console.log('# starting map via console…');
await page.keyboard.press('Backquote');
await sleep(400);
await page.keyboard.type('map start', { delay: 40 });
await page.keyboard.press('Enter');
await sleep(8000); /* map load (loading = forced console = quad mode meanwhile) */
await page.keyboard.press('Backquote'); /* make sure console is closed */
await sleep(600);
st = await probe();
console.log('  probe (in-game):', JSON.stringify(st));
check('in-game: quad path OFF (normal head-tracked stereo + HUD)',
  st.quad === 0 && st.demo === 0 && st.bigScreen === 0 && st.mState === 0,
  JSON.stringify(st));
eyes = await probeEyes();
console.log('  eyes (in-game):', JSON.stringify(eyes));
for (const e of eyes) {
  const lit = e.corners.filter((c) => c > 8).length + (e.center > 8 ? 1 : 0);
  check(`${e.eye}: world render fills the eye viewport in-game`, lit >= 3,
    `center=${e.center} corners=${JSON.stringify(e.corners)}`);
}
await page.screenshot({ path: SHOT + '-ingame-stereo.png' });

/* ---- 7) menu over the game world: quad again, world+menu on it ---- */
await leftStickClick();
st = await probe();
check('in-game menu opens as quad (m_state=m_main, quad active)',
  st.mState === 1 && st.quad === 1, JSON.stringify(st));
await page.screenshot({ path: SHOT + '-menu-ingame.png' });
await leftStickClick(); /* close again */

/* ---- 8) exit VR cleanly ---- */
await page.evaluate(() => document.getElementById('entervr').click());
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 15000 });
await sleep(500);
st = await probe();
check('flatscreen after exit: quad path off', st.quad === 0, JSON.stringify(st));
await page.screenshot({ path: SHOT + '-flatscreen-after.png' });

const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
console.log('\n# console errors: ' + realErrors.length);
realErrors.forEach((e) => console.log('  ERR: ' + e));

await browser.close();
const ok = fails.length === 0 && realErrors.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + [...fails, ...realErrors].join('; ') + ')'));
process.exit(ok ? 0 : 1);
