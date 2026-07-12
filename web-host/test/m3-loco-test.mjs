#!/usr/bin/env node
/*
 * m3-loco-test.mjs — emulated-XR verification of M3 chunk 1 (locomotion &
 * turning), extending the M3 foundation's test harness pattern
 * (m3-input-test.mjs): same IWER setup, drive emulated controllers, assert
 * on live state — but this one asserts the PLAYER ACTUALLY MOVES/TURNS
 * in-game (cl.viewangles/cl.comfortInc/cl.movement_origin via the
 * ?locodebug=1 vr_locodebug overlay, formatted in C from the C-side
 * client_state_t so a match proves the WebXR->engine locomotion pipeline
 * works end-to-end), not just that input bits crossed JS->C.
 *
 * Boot lands in demo1 playback (shareware default startdemos), where the
 * local player entity doesn't respond to movement input. This test opens
 * the in-engine console (backtick) and types "map start" — exactly what a
 * flatscreen user would do — to drop into a live, controllable game before
 * exercising locomotion. No engine/source changes needed for this; it's the
 * same keyboard path M1 verified (on_key -> QC_KeyEvent -> Key_Event).
 *
 * Prereqs / env overrides: see m3-input-test.mjs header (same variables).
 * Usage: node m3-loco-test.mjs [screenshot.png]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
/* M3 integration: default to the merged build served by `node web/serve.mjs
 * 8090` (was 8092 = this chunk's private worktree webdist server). */
const URL = process.env.QQ_URL || 'http://localhost:8090/?autostart=1&locodebug=1&inputdebug=1';
const SCREENSHOT = process.argv[2] || 'm3-loco-emulated.png';

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
const locoOverlay = () => page.evaluate(() =>
  (document.getElementById('qq-locodebug') || {}).textContent || '');

const parseLoco = (t) => {
  const va = t.match(/viewangles\(p=\s*(-?[\d.]+)\s+y=\s*(-?[\d.]+)\s+r=\s*(-?[\d.]+)\)/);
  const ci = t.match(/comfortInc=(-?\d+)/);
  const org = t.match(/origin\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\)/);
  if (!va || !ci || !org) return null;
  return {
    pitch: +va[1], yaw: +va[2], roll: +va[3],
    comfortInc: +ci[1],
    origin: [+org[1], +org[2], +org[3]],
  };
};
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const angDelta = (a, b) => { // shortest signed delta a->b in degrees
  let d = b - a;
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return d;
};

console.log('# loading ' + URL);
await page.goto(URL, { waitUntil: 'load' });

await page.waitForFunction(
  () => document.getElementById('start')?.textContent === 'Running', { timeout: 30000 });
console.log('# engine running; letting boot/demo settle…');
await sleep(4000);

await page.waitForFunction(
  () => !document.getElementById('entervr')?.disabled, { timeout: 15000 });
await page.click('#entervr');
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 15000 });
console.log('# immersive session active');
await sleep(1000);

// ---- vr_yawmode session-start restore (WebXRLoco_OnSessionStart) ----
let txt = await locoOverlay();
console.log('--- loco overlay right after VR entry ---\n' + txt + '------------------------------------------');
check('vr_locodebug overlay present', txt.includes('vr loco'));
check('vr_yawmode restored to 1 (snap) on VR entry', /vr_yawmode=1\b/.test(txt), txt.match(/vr_yawmode=\d+/)?.[0]);

// ---- escape demo playback into a live, controllable game: open console,
// type "map start" (same keyboard path a flatscreen player uses) ----
// Reused before every displacement-measuring phase below: it resets the
// player to the same deterministic, open spawn point/orientation each time,
// so distance-covered comparisons (e.g. walk vs. walk+run) aren't
// contaminated by cumulative drift into walls/corners/geometry left over
// from earlier phases — an actual failure mode hit during development (a
// run-vs-no-run comparison came out inverted because the "run" phase started
// ~340 units from spawn, into unknown geometry, after two prior displacement
// tests had pushed the player there; see report).
async function resetToSpawn(label) {
  await page.keyboard.press('Backquote');
  await sleep(200);
  await page.keyboard.type('map start', { delay: 20 });
  await page.keyboard.press('Enter');
  await sleep(300);
  // IMPORTANT: single-player freezes the sim clock while the console is open
  // (host.c:971 — cl.islocalgame && key_consoleactive => clframetime = 0), so
  // leaving it open would silently zero every locomotion test below. Close it.
  await page.keyboard.press('Backquote');
  console.log(`# issued "map start" via console (${label || 'reset'}, console closed)`);
  await sleep(2200);
}
await resetToSpawn('leave demo playback');

let loco0 = parseLoco(await locoOverlay());
check('loco state parses after map start', !!loco0, JSON.stringify(loco0));
console.log('baseline: ' + JSON.stringify(loco0));

// =====================================================================
// 1) Snap-turn: right-stick flick -> cl.comfortInc -> cl.viewangles[YAW]
// =====================================================================
await page.evaluate(() => window.__xrdevice.controllers.right.updateAxes('thumbstick', 0.8, 0));
await sleep(200);
await page.evaluate(() => window.__xrdevice.controllers.right.updateAxes('thumbstick', 0, 0));
await sleep(300); // let canAdjust latch reset (QC_MotionEvent: fabs(dx) < 0.3)

let loco1 = parseLoco(await locoOverlay());
console.log('after flick 1 (+x): ' + JSON.stringify(loco1));
check('comfortInc changed after right-stick flick #1', loco1.comfortInc !== loco0.comfortInc,
  `${loco0.comfortInc} -> ${loco1.comfortInc}`);
const yawDelta1 = angDelta(loco0.yaw, loco1.yaw);
check('viewangles[YAW] snapped by ~45 deg (cl_comfort default) after flick #1',
  Math.abs(Math.abs(yawDelta1) - 45) < 2, `yaw delta = ${yawDelta1.toFixed(1)}`);

// second flick, opposite stick direction, to prove it's not a one-shot fluke
await page.evaluate(() => window.__xrdevice.controllers.right.updateAxes('thumbstick', -0.8, 0));
await sleep(200);
await page.evaluate(() => window.__xrdevice.controllers.right.updateAxes('thumbstick', 0, 0));
await sleep(300);

let loco2 = parseLoco(await locoOverlay());
console.log('after flick 2 (-x): ' + JSON.stringify(loco2));
check('comfortInc changed again after right-stick flick #2', loco2.comfortInc !== loco1.comfortInc,
  `${loco1.comfortInc} -> ${loco2.comfortInc}`);
const yawDelta2 = angDelta(loco1.yaw, loco2.yaw);
check('viewangles[YAW] snapped by ~45 deg after flick #2',
  Math.abs(Math.abs(yawDelta2) - 45) < 2, `yaw delta = ${yawDelta2.toFixed(1)}`);
check('flick #2 turned the opposite way from flick #1', Math.sign(yawDelta1) !== 0 && Math.sign(yawDelta1) === -Math.sign(yawDelta2),
  `sign1=${Math.sign(yawDelta1)} sign2=${Math.sign(yawDelta2)}`);

// =====================================================================
// 2) Smooth thumbstick locomotion: left-stick forward -> player origin moves
// =====================================================================
let locoBeforeWalk = parseLoco(await locoOverlay());
await page.evaluate(() => window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, -1.0));
await sleep(1200);
await page.evaluate(() => window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, 0));
await sleep(300);
let locoAfterWalk = parseLoco(await locoOverlay());
const walkDist = dist(locoBeforeWalk.origin, locoAfterWalk.origin);
console.log(`walk: ${JSON.stringify(locoBeforeWalk.origin)} -> ${JSON.stringify(locoAfterWalk.origin)} (dist=${walkDist.toFixed(1)})`);
check('left-thumbstick forward moved the player (origin delta > 20 units)', walkDist > 20, `dist=${walkDist.toFixed(1)}`);

// Confirm movement decays (Quake ground friction, cl_movement_friction) once
// the stick is released, rather than coasting on unchecked. Momentum means
// it won't stop *instantly* — one or two frames of residual velocity are
// expected — so this compares the post-release drift against an equivalent
// held-window distance (extrapolated from the walk above) instead of a bare
// absolute threshold: it must be markedly smaller, proving deceleration
// happened, without being a flaky guess at exact friction timing.
let locoSettle1 = parseLoco(await locoOverlay());
await sleep(500);
let locoSettle2 = parseLoco(await locoOverlay());
const driftDist = dist(locoSettle1.origin, locoSettle2.origin);
const heldEquivalent = (walkDist / 1200) * 500; // distance if still moving at walk speed for 500ms
check('player decelerates sharply once thumbstick is released (vs. continuing at walk speed)',
  driftDist < heldEquivalent * 0.5,
  `dist=${driftDist.toFixed(1)} vs held-equivalent=${heldEquivalent.toFixed(1)}`);

// =====================================================================
// 3) HMD positional-delta walking: lean/step the HMD with the stick neutral
// =====================================================================
await resetToSpawn('before positional-delta phase');
let locoBeforeLean = parseLoco(await locoOverlay());
await page.evaluate(() => { window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, 0); });
for (let i = 1; i <= 24; i++) {
  const z = -0.2 - i * 0.03; // step "backward" in XR space repeatedly
  await page.evaluate((zz) => { window.__xrdevice.position.set(0, 1.6, zz); }, z);
  await sleep(60);
}
await sleep(300);
let locoAfterLean = parseLoco(await locoOverlay());
const leanDist = dist(locoBeforeLean.origin, locoAfterLean.origin);
console.log(`positional-delta walk: ${JSON.stringify(locoBeforeLean.origin)} -> ${JSON.stringify(locoAfterLean.origin)} (dist=${leanDist.toFixed(1)})`);
check('HMD positional-delta stepping moved the player (origin delta > 5 units)', leanDist > 5, `dist=${leanDist.toFixed(1)}`);

// reset HMD back near origin for cleanliness
await page.evaluate(() => { window.__xrdevice.position.set(0, 1.6, 0); });
await sleep(200);

// =====================================================================
// 4) Off-hand run trigger (+speed): compare walk distance with/without.
// Each sub-phase gets its own fresh spawn (resetToSpawn) so both start from
// the *identical* origin/orientation/open geometry — otherwise whichever
// phase runs second inherits drift from the first (and possibly a wall/
// corner) and the comparison is meaningless noise, not a speed measurement.
// =====================================================================
await resetToSpawn('before no-run phase');
let baseA = parseLoco(await locoOverlay());
await page.evaluate(() => window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, -1.0));
await sleep(800);
await page.evaluate(() => window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, 0));
await sleep(300);
let baseB = parseLoco(await locoOverlay());
const walkNoRun = dist(baseA.origin, baseB.origin);

await resetToSpawn('before run phase');
let runA = parseLoco(await locoOverlay());
await page.evaluate(() => {
  window.__xrdevice.controllers.left.updateButtonValue('trigger', 1.0);
  window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, -1.0);
});
await sleep(800);
await page.evaluate(() => {
  window.__xrdevice.controllers.left.updateAxes('thumbstick', 0, 0);
  window.__xrdevice.controllers.left.updateButtonValue('trigger', 0);
});
await sleep(300);
let runB = parseLoco(await locoOverlay());
const walkRun = dist(runA.origin, runB.origin);
console.log(`walk speed: no-run=${walkNoRun.toFixed(1)} run=${walkRun.toFixed(1)}`);
check('off-hand trigger (+speed/run) increases walk distance', walkRun > walkNoRun * 1.15,
  `no-run=${walkNoRun.toFixed(1)} run=${walkRun.toFixed(1)}`);

await page.screenshot({ path: SCREENSHOT });
console.log('# screenshot: ' + SCREENSHOT);

// ---- exit VR: flatscreen resumes cleanly (regression) ----
await page.evaluate(() => document.getElementById('entervr').click());
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 15000 });
await sleep(500);
check('flatscreen resumed after exiting VR', await page.evaluate(() => !document.getElementById('entervr').disabled));

const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
console.log('\n# console errors: ' + realErrors.length);
realErrors.forEach((e) => console.log('  ERR: ' + e));

await browser.close();
const ok = fails.length === 0 && realErrors.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + [...fails, ...realErrors].join('; ') + ')'));
process.exit(ok ? 0 : 1);
