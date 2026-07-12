#!/usr/bin/env node
/*
 * m3-comfort-test.mjs — emulated-XR verification of M3 chunk 4
 * (comfort/calibration: recenter, quicksave/quickload, bullet-time,
 * vr_worldscale). Same IWER/puppeteer harness pattern as m3-input-test.mjs
 * (see its header comment).
 *
 * Does NOT test laser-sight toggle or weapon-switch stick-flick: both were
 * implemented here originally, then removed after cross-checking sibling
 * chunk reports found chunk 2 (m3-weapon, reports/08b-weapon.md) already
 * owns them and explicitly warns against duplicating the weapon-switch
 * flick ("dupe symptom: double weapon switches per flick") — see
 * reports/08d-comfort.md "Discovered cross-chunk duplication". The
 * quicksave/quickload round-trip below still needs *some* way to change
 * the active weapon to make the round-trip observable; it uses a typed
 * `impulse 12` console command for that (decoupled from whichever chunk
 * ends up owning the stick-flick binding at merge time).
 *
 * Boots straight into a real single-player game (?startargs=+map,start)
 * instead of the default demo loop, so sv.active is true and
 * save/load (quicksave/quickload) actually exercise the engine's
 * Host_Savegame_f/Host_Loadgame_f path instead of failing on "no server
 * running".
 *
 * Prereqs / env overrides: see m3-input-test.mjs header.
 * Usage: node m3-comfort-test.mjs [screenshot.png]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
/* M3 integration: default to the merged build served by `node web/serve.mjs
 * 8090` (was 8095 = this chunk's private worktree webdist server);
 * ?startargs= passthrough is now native to web/index.html. */
const URL = process.env.QQ_URL
  || 'http://localhost:8090/?autostart=1&inputdebug=1&startargs=%2Bmap,start';
const SCREENSHOT = process.argv[2] || 'm3-comfort-emulated.png';

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
const diag = [];
page.on('console', (m) => {
  if (m.type() === 'error') errors.push(m.text());
  if (/recenter|comfort|webxr|impulse|unbound|not bound/i.test(m.text())) diag.push(`[${m.type()}] ${m.text()}`);
});
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
const overlay = () => page.evaluate(() =>
  (document.getElementById('qq-inputdebug') || {}).textContent || '');
const hmdOf = (t) => {
  const m = t.match(/hmd pos\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\)\s+ypr\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\)/);
  return m ? { x: +m[1], y: +m[2], z: +m[3], pitch: +m[4], yaw: +m[5], roll: +m[6] } : null;
};

console.log('# loading ' + URL);
await page.goto(URL, { waitUntil: 'load' });

await page.waitForFunction(
  () => document.getElementById('start')?.textContent === 'Running', { timeout: 30000 });
console.log('# engine running (+map start); letting it settle…');
await sleep(3000);

await page.waitForFunction(
  () => !document.getElementById('entervr')?.disabled, { timeout: 15000 });
await page.click('#entervr');
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 15000 });
console.log('# immersive session active');
await sleep(1500);

// ---- vr_worldscale: engine-side cvar path end-to-end ----
// (default 26.2467 per gl_rmain.c; confirms the cvar registered, is
// readable through the M3-comfort debug probe, and the VR options menu's
// 30-400 slider range brackets the default sanely)
let ws = await page.evaluate(() => Module._WebXRComfort_DebugGetWorldscale());
check('vr_worldscale default ~26.2467', Math.abs(ws - 26.2467) < 0.01, 'ws=' + ws);

// ================= Test A: recenter (yaw + position) =================
// NOTE ON WHAT THIS CAN VERIFY UNDER IWER: IWER 2.3.0's
// XRReferenceSpace.getOffsetReferenceSpace expects a raw mat4, not the
// spec-mandated XRRigidTransform (iwer/lib/spaces/XRReferenceSpace.d.ts:26
// vs iwer/lib/spaces/XRSpace.js:17's mat4.clone(offsetMatrix) — see
// PATCHES.md #14). Passing a real XRRigidTransform (the only spec-correct
// input, and what webxr_recenter() does) makes IWER's pose math silently
// no-op instead of erroring, so the emulated headset's *reported pose*
// never actually moves to the new origin — a harness limitation, not a
// port defect. This block therefore verifies everything that CAN be
// verified under emulation: the button/console-command trigger the C-side
// recenter path (edge detection fires, engine logs "recentered", height
// re-latches), not the numeric pose zeroing. The transform math itself
// (position X/Z untransformed + Y zeroed, yaw-only twist quaternion) was
// separately verified correct via a standalone gl-matrix reimplementation
// of IWER's own calculateGlobalOffsetMatrix/getViewerPose algebra — see
// reports/08d-comfort.md.
await page.evaluate(() => {
  const dev = window.__xrdevice;
  dev.position.set(2.0, 1.6, 3.0);
  const q = IWER.eulerToQuat({ yaw: 45 });
  dev.quaternion.x = q.x; dev.quaternion.y = q.y; dev.quaternion.z = q.z; dev.quaternion.w = q.w;
});
await sleep(600);
let txt = await overlay();
let hmd0 = hmdOf(txt);
console.log('--- after moving device (pre-recenter) ---\n' + (txt.split('\n')[1] || ''));
check('device move reflected (position)', !!hmd0 && Math.abs(hmd0.x - 2.0) < 0.1 && Math.abs(hmd0.z - 3.0) < 0.1, JSON.stringify(hmd0));
check('device move reflected (yaw != 0)', !!hmd0 && Math.abs(hmd0.yaw) > 5, 'yaw=' + (hmd0 && hmd0.yaw));

const diagCountBefore = () => diag.length;
const sawRecenterLog = (from) => diag.slice(from).some((d) => d.includes('[comfort] recentered'));

// off-hand (LEFT, default cl_righthanded=1) thumbstick click LONG-press
// (>= 600 ms) = recenter. M3 integration change: the plain click collided
// with chunk 3's menu toggle in the merged build; in_menu.c now owns the
// gesture (short press = menu, long press = recenter) — see
// reports/09-m3-integration.md.
let markA = diagCountBefore();
await page.evaluate(() => {
  window.__xrdevice.controllers.left.updateButtonValue('thumbstick', 1.0);
});
await sleep(1000); // hold well past the 600 ms long-press threshold
await page.evaluate(() => {
  window.__xrdevice.controllers.left.updateButtonValue('thumbstick', 0.0);
});
await sleep(600);
check('off-hand thumbstick LONG-press fires the recenter path', sawRecenterLog(markA));
// the long press must NOT have toggled the menu (that's the short press)
{
  const st = await page.evaluate(() => Module._VRMenuQuad_DebugState());
  check('long-press did not open the menu (m_state stays m_none)',
    ((st >> 16) & 0xff) === 0, 'probe=0x' + st.toString(16));
}

// vr_recenter console command (fallback trigger) should also fire the same
// path. Move the device again first so it's a fresh, distinguishable click.
await page.evaluate(() => {
  const dev = window.__xrdevice;
  dev.position.set(-1.5, 1.7, 0.8);
});
await sleep(400);
let markB = diagCountBefore();
await page.keyboard.press('Backquote');
await sleep(200);
await page.keyboard.type('vr_recenter');
await page.keyboard.press('Enter');
await sleep(200);
await page.keyboard.press('Backquote');
await sleep(600);
check('vr_recenter console command fires the recenter path', sawRecenterLog(markB));

// Test B (laser-sight toggle) and Test C (weapon-switch stick-flick) are
// intentionally not here — see the file header and
// reports/08d-comfort.md; both are chunk 2's (verified in
// reports/08b-weapon.md).

// ================= Test D: quicksave / quickload round-trip =================
async function typeConsole(cmd) {
  await page.keyboard.press('Backquote');
  await sleep(150);
  await page.keyboard.type(cmd);
  await page.keyboard.press('Enter');
  await sleep(150);
  await page.keyboard.press('Backquote');
  await sleep(300);
}

// left X = quicksave (edge-triggered) — save the starting-loadout weapon
// as the known-good state.
let w0 = await page.evaluate(() => Module._WebXRComfort_DebugGetActiveWeapon());
await page.evaluate(() => window.__xrdevice.controllers.left.updateButtonValue('x-button', 1.0));
await sleep(700);
await page.evaluate(() => window.__xrdevice.controllers.left.updateButtonValue('x-button', 0.0));
await sleep(700);
let wSaved = await page.evaluate(() => Module._WebXRComfort_DebugGetActiveWeapon());
check('quicksave triggered with the expected starting weapon', wSaved === w0, `${w0} -> ${wSaved}`);
console.log('# quicksaved with active weapon = ' + wSaved);

// change weapon via a typed console command (decoupled from whichever
// chunk owns the controller-driven weapon-switch binding) so load has
// something observable to restore. "impulse 12" empirically cycles
// shotgun(1) -> axe(4096) on the fresh-game loadout (verified while this
// module still owned weapon-switch — see reports/08d-comfort.md).
await typeConsole('impulse 12');
await sleep(300);
let wChanged = await page.evaluate(() => Module._WebXRComfort_DebugGetActiveWeapon());
check('weapon changed after save (setup for load assertion)', wChanged !== wSaved, `${wSaved} -> ${wChanged}`);

// left Y = quickload (edge-triggered)
await page.evaluate(() => window.__xrdevice.controllers.left.updateButtonValue('y-button', 1.0));
await sleep(900);
await page.evaluate(() => window.__xrdevice.controllers.left.updateButtonValue('y-button', 0.0));
await sleep(900);
let wLoaded = await page.evaluate(() => Module._WebXRComfort_DebugGetActiveWeapon());
check('quickload restored the saved active weapon', wLoaded === wSaved, `expected ${wSaved}, got ${wLoaded}`);

await page.screenshot({ path: SCREENSHOT });
console.log('# screenshot: ' + SCREENSHOT);

// ---- exit VR: flatscreen resumes cleanly ----
await page.evaluate(() => document.getElementById('entervr').click());
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 15000 });
await sleep(500);
console.log('# flatscreen resumed after exit');

console.log('\n# diag (recenter/comfort/webxr console lines): ' + diag.length);
diag.forEach((d) => console.log('  ' + d));

const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
console.log('\n# console errors: ' + realErrors.length);
realErrors.forEach((e) => console.log('  ERR: ' + e));

await browser.close();
const ok = fails.length === 0 && realErrors.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + [...fails, ...realErrors].join('; ') + ')'));
process.exit(ok ? 0 : 1);
