#!/usr/bin/env node
/*
 * m3-input-test.mjs — emulated-XR verification of the M3 input foundation
 * (and a reusable pattern for the four M3 chunk agents: same IWER setup,
 * drive controllers, assert on the ?inputdebug=1 overlay, whose text is
 * formatted IN C from the C-side structs — so a match proves the data
 * crossed the JS->C boundary).
 *
 * Prereqs:
 *   - build served locally:      node web/serve.mjs 8090
 *   - iwer installed somewhere:  npm install iwer   (v2.x)
 *   - puppeteer-core available (the webxr-test skill folder has it)
 *
 * Env overrides:
 *   QQ_URL           (default http://localhost:8090/?autostart=1&inputdebug=1)
 *   QQ_IWER_JS       path to iwer/build/iwer.min.js
 *   QQ_PUPPETEER     path to a puppeteer-core install dir (containing lib/esm/...)
 *   QQ_CHROME        Chrome/Edge executable
 *
 * Usage: node m3-input-test.mjs [screenshot.png]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
const URL = process.env.QQ_URL || 'http://localhost:8090/?autostart=1&inputdebug=1';
const SCREENSHOT = process.argv[2] || 'm3-input-emulated.png';

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

// install IWER before any page script runs (forceInstall: headless Chrome has
// a native navigator.xr that reports immersive-vr unsupported — M2 notes)
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
await sleep(1500);

// ---- baseline: controllers present, poses valid, no buttons ----
let txt = await overlay();
console.log('--- baseline overlay ---\n' + txt + '------------------------');
check('overlay present', txt.includes('vr input (session=1)'));
check('left hand present', /^L: grip/m.test(txt));
check('right hand present', /^R: grip/m.test(txt));
check('no stale buttons at rest', /btn=00000000/.test(txt));
check('gamepads connected', (txt.match(/gp=1/g) || []).length === 2);
check('haptic actuators seen', (txt.match(/hap=1/g) || []).length === 2);

const aimOf = (t, hand) => {
  const m = t.match(new RegExp('^' + hand + ':.*aim.?\\(\\s*(-?[\\d.]+)\\s+(-?[\\d.]+)\\s+(-?[\\d.]+)\\)', 'm'));
  return m ? [+m[1], +m[2], +m[3]] : null;
};
const aim0 = aimOf(txt, 'R');
check('right aim pose parsed', !!aim0, JSON.stringify(aim0));

// ---- drive the emulated controllers ----
await page.evaluate(() => {
  const dev = window.__xrdevice;
  const R = dev.controllers.right, L = dev.controllers.left;
  R.position.set(0.35, 1.1, -0.45);
  L.position.set(-0.3, 1.2, -0.2);
  // right: trigger pressed, thumbstick pushed UP (xr-standard y = -0.8),
  // B pressed, A touched
  R.updateButtonValue('trigger', 1.0);
  R.updateAxes('thumbstick', 0.5, -0.8);
  R.updateButtonValue('b-button', 1.0);
  R.updateButtonTouch('a-button', true);
  // left: squeeze, X pressed, thumbstick click
  L.updateButtonValue('squeeze', 1.0);
  L.updateButtonValue('x-button', 1.0);
  L.updateButtonValue('thumbstick', 1.0);
});
await sleep(600); // several XR frames + >200ms overlay refresh

txt = await overlay();
console.log('--- driven overlay ---\n' + txt + '----------------------');

const rBtn = (txt.match(/^R:[\s\S]*?btn=([0-9a-f]{8})/m) || [])[1];
const rTch = (txt.match(/^R:[\s\S]*?tch=([0-9a-f]{8})/m) || [])[1];
const lBtn = (txt.match(/^L:[\s\S]*?btn=([0-9a-f]{8})/m) || [])[1];
check('R trigger bit set (0x20000000)', rBtn && (parseInt(rBtn, 16) & 0x20000000) !== 0, 'btn=' + rBtn);
check('R B-button bit set (0x2)', rBtn && (parseInt(rBtn, 16) & 0x2) !== 0);
check('R A touch bit set (0x1)', rTch && (parseInt(rTch, 16) & 0x1) !== 0, 'tch=' + rTch);
check('R trigger analog = 1.00', /^R:[\s\S]*?trig=1\.00/m.test(txt));
check('R stick x=0.5 (xr-standard axes[2])', /^R:[\s\S]*?stick\(\s*0\.50/m.test(txt));
check('R stick y=+0.8 (sign flipped to OpenXR convention)', /^R:[\s\S]*?stick\(\s*0\.50\s+0\.80\)/m.test(txt));
check('L grip bit set (0x04000000)', lBtn && (parseInt(lBtn, 16) & 0x04000000) !== 0, 'btn=' + lBtn);
check('L X bit set (0x100)', lBtn && (parseInt(lBtn, 16) & 0x100) !== 0);
check('L thumbstick-click sets LThumb|Joystick', lBtn && ((parseInt(lBtn, 16) & 0x80000400) >>> 0) === 0x80000400);
check('L grip analog = 1.00', /^L:[\s\S]*?grip=1\.00/m.test(txt));

const aim1 = aimOf(txt, 'R');
check('R aim pose reflects driven position', !!aim1 &&
  Math.abs(aim1[0] - 0.35) < 0.05 && Math.abs(aim1[1] - 1.1) < 0.1 && Math.abs(aim1[2] + 0.45) < 0.05,
  JSON.stringify(aim1));
check('grip pose also valid', /^R: grip \(/m.test(txt) || /^R: grip\(/m.test(txt));

// ---- haptics: C-side channel API against IWER's emulated actuator ----
const hap = await page.evaluate(() => {
  const r1 = Module._WebXRInput_HapticPulse(1, 0.8, 120); // raw pulse, right
  Module._WebXRInput_Vibrate(150, 3, 1.0);                // both channels
  Module._WebXRInput_Vibrate(500, 2, 0.5);                // right busy -> rejected (fork semantics)
  return { rawPulse: r1 };
});
check('raw haptic pulse accepted (emulated actuator)', hap.rawPulse === 1);

// ---- release buttons; verify state clears (no latching) ----
await page.evaluate(() => {
  const dev = window.__xrdevice;
  dev.controllers.right.updateButtonValue('trigger', 0);
  dev.controllers.right.updateAxes('thumbstick', 0, 0);
  dev.controllers.right.updateButtonValue('b-button', 0);
  dev.controllers.right.updateButtonTouch('a-button', false);
  dev.controllers.left.updateButtonValue('squeeze', 0);
  dev.controllers.left.updateButtonValue('x-button', 0);
  dev.controllers.left.updateButtonValue('thumbstick', 0);
});
await sleep(600);
txt = await overlay();
check('buttons clear on release', (txt.match(/btn=00000000/g) || []).length === 2);

await page.screenshot({ path: SCREENSHOT });
console.log('# screenshot: ' + SCREENSHOT);

// ---- exit VR: flatscreen resumes, input state resets ----
// (DOM click: puppeteer's trusted second mouse-click on the same button
// proved flaky headless; the handler is what's under test)
await page.evaluate(() => document.getElementById('entervr').click());
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 15000 });
await sleep(500);
txt = await overlay();
check('input state reset on session end', txt.includes('session=0'), txt.split('\n')[0]);

const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
console.log('\n# console errors: ' + realErrors.length);
realErrors.forEach((e) => console.log('  ERR: ' + e));

await browser.close();
const ok = fails.length === 0 && realErrors.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + [...fails, ...realErrors].join('; ') + ')'));
process.exit(ok ? 0 : 1);
