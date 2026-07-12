#!/usr/bin/env node
/*
 * m3-integration-test.mjs — MERGED-build cross-chunk interaction checks the
 * four per-chunk M3 suites (m3-{loco,weapon,hud,comfort}-test.mjs) could not
 * see in isolation (reports/09-m3-integration.md):
 *
 *  (2b) menu open while moving: hold the movement stick, short-click the
 *       off-hand thumbstick -> menu quad opens and the PLAYER STOPS (the
 *       engine freezes the local-game sim clock while key_dest != game,
 *       host.c:882/971); close -> movement resumes with the stick still held.
 *  (2c) off-hand thumbstick-click collision resolution (chunk 3 menu toggle
 *       vs chunk 4 recenter — both picked the same input): SHORT press
 *       toggles the menu and does NOT recenter; LONG press (>= 600 ms,
 *       in-game) recenters (with a haptic confirm on the off hand) and does
 *       NOT toggle the menu.
 *  (2d) quicksave (keyboard F6; the X binding was removed in headset-QA
 *       round 2) -> weapon flick -> quickload (F9) mid-game with the weapon
 *       aimed and the TRIGGER HELD ACROSS THE LOAD: saved state restores,
 *       no stuck +attack / haptics after release, controller aim still
 *       drives gunangles.
 *
 * Prereqs: merged build served on 8090 (node web/serve.mjs 8090 from the
 * webxr-port root). Env overrides: see m3-input-test.mjs header.
 * Usage: node m3-integration-test.mjs [screenshot.png]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
const URL = process.env.QQ_URL || 'http://localhost:8090/?autostart=1&locodebug=1&args='
  + encodeURIComponent('+skill 0 +map start');
const SCREENSHOT = process.argv[2] || 'm3-integration.png';

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
page.setDefaultTimeout(45000);

const errors = [];
const diag = [];
page.on('console', (m) => {
  if (m.type() === 'error') errors.push(m.text());
  if (/recenter|comfort|webxr/i.test(m.text())) diag.push(m.text());
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
  console.log(`  [${cond ? 'PASS' : 'FAIL'}] ${name}${detail !== undefined ? ' — ' + detail : ''}`);
  if (!cond) fails.push(name);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/* probes (same surfaces the per-chunk suites use) */
const P = { GA_Y: 1, WEAPON: 12, SHELLS: 13, HEALTH: 14, SIGNON: 15,
  HAPCNT: 17, AIMACT: 20 };
const probe = (i) => page.evaluate((i) => Module._IN_Weapon_Probe(i), i);
const menuSt = async () => {
  const v = await page.evaluate(() => Module._VRMenuQuad_DebugState());
  return { raw: v, quad: v & 1, bigScreen: (v >> 4) & 0xf, mState: (v >> 16) & 0xff };
};
const locoOrigin = async () => {
  const t = await page.evaluate(() =>
    (document.getElementById('qq-locodebug') || {}).textContent || '');
  const m = t.match(/origin\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\)/);
  return m ? [+m[1], +m[2], +m[3]] : null;
};
const dist = (a, b) => Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
const sawRecenter = (from) => diag.slice(from).some((d) => d.includes('[comfort] recentered'));
const ctrl = (hand, fn, ...a) => page.evaluate(([h, f, args]) => {
  const c = window.__xrdevice.controllers[h];
  c[f](...args); return null;
}, [hand, fn, a]);
const shortClick = async () => { /* off-hand (left) thumbstick, < 600 ms */
  await ctrl('left', 'updateButtonValue', 'thumbstick', 1.0);
  await sleep(220);
  await ctrl('left', 'updateButtonValue', 'thumbstick', 0.0);
  await sleep(450);
};

console.log('# loading ' + URL);
await page.goto(URL, { waitUntil: 'load' });
await page.waitForFunction(
  () => document.getElementById('start')?.textContent === 'Running', { timeout: 45000 });
await page.waitForFunction(
  () => Module._IN_Weapon_Probe && Module._IN_Weapon_Probe(15) === 4 && Module._IN_Weapon_Probe(14) > 0,
  { timeout: 45000 });
console.log('# map start up (health=' + await probe(P.HEALTH) + ')');

await page.waitForFunction(() => !document.getElementById('entervr')?.disabled);
await page.click('#entervr');
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 20000 });
await sleep(1500);
console.log('# immersive session active');

/* enable the loco debug overlay's C-side source + spy haptic actuators */
await page.evaluate(() => {
  window.__pulses = [];
  for (const src of Module.webxr_session.inputSources) {
    const act = src.gamepad && src.gamepad.hapticActuators && src.gamepad.hapticActuators[0];
    if (act && !act.__spied) {
      act.__spied = true;
      const orig = act.pulse.bind(act);
      act.pulse = (i, d) => { window.__pulses.push({ hand: src.handedness, i, d }); return orig(i, d); };
    }
  }
  return null;
});

/* ================= (2c) short press: menu toggles, NO recenter ========= */
let mark = diag.length;
await shortClick();
let st = await menuSt();
check('2c short click opens the menu (m_state=m_main, quad on)',
  st.mState === 1 && st.quad === 1, JSON.stringify(st));
check('2c short click did NOT recenter', !sawRecenter(mark));
await shortClick();
st = await menuSt();
check('2c second short click closes the menu (m_state=m_none, bigScreen=0)',
  st.mState === 0 && st.bigScreen === 0, JSON.stringify(st));
check('2c menu close click did NOT recenter either (stale-edge regression)',
  !sawRecenter(mark));

/* ================= (2c) long press: recenter, NO menu ================== */
mark = diag.length;
await ctrl('left', 'updateButtonValue', 'thumbstick', 1.0);
await sleep(1000); /* past the 600 ms threshold */
await ctrl('left', 'updateButtonValue', 'thumbstick', 0.0);
await sleep(500);
check('2c long press fires recenter', sawRecenter(mark));
st = await menuSt();
check('2c long press did NOT toggle the menu', st.mState === 0 && st.bigScreen === 0,
  JSON.stringify(st));
const pulsesLeft = await page.evaluate(() =>
  window.__pulses.filter((p) => p.hand === 'left').length);
check('2c long press gave a haptic confirm on the off hand', pulsesLeft > 0,
  'left pulses=' + pulsesLeft);

/* ================= (2b) menu while moving ============================== */
await ctrl('left', 'updateAxes', 'thumbstick', 0, -1.0); /* forward */
await sleep(900);
let o1 = await locoOrigin(); await sleep(600); let o2 = await locoOrigin();
check('2b stick-forward moves the player pre-menu', o1 && o2 && dist(o1, o2) > 10,
  o1 && o2 ? 'd=' + dist(o1, o2).toFixed(1) : 'no overlay');

await shortClick(); /* menu opens with the stick STILL held */
st = await menuSt();
check('2b menu opened while moving', st.mState === 1 && st.quad === 1, JSON.stringify(st));
let m1 = await locoOrigin(); await sleep(700); let m2 = await locoOrigin();
check('2b movement FROZEN while menu is up (sim clock gated)',
  m1 && m2 && dist(m1, m2) < 1.0, m1 && m2 ? 'd=' + dist(m1, m2).toFixed(2) : 'no overlay');

await shortClick(); /* close, stick still held */
st = await menuSt();
check('2b menu closed again', st.mState === 0 && st.bigScreen === 0, JSON.stringify(st));
await sleep(600);
let r1 = await locoOrigin(); await sleep(600); let r2 = await locoOrigin();
check('2b movement resumes after close (stick was held throughout)',
  r1 && r2 && dist(r1, r2) > 10, r1 && r2 ? 'd=' + dist(r1, r2).toFixed(1) : 'no overlay');
await ctrl('left', 'updateAxes', 'thumbstick', 0, 0);
await sleep(400);

/* ================= (2d) quicksave/quickload with weapon aimed ========== */
await page.evaluate(() => {
  const dev = window.__xrdevice;
  dev.controllers.right.position.set(0.3, 1.2, -0.4);
  dev.controllers.right.quaternion.set(0, 0, 0, 1);
  return null;
});
await sleep(600);
check('2d controller aim active before save', await probe(P.AIMACT) === 1);

/* QA round 2: X/Y quicksave/quickload bindings are REMOVED (accidental
 * save/load veto) — drive the same engine path via the keyboard F6/F9
 * binds from the shareware default.cfg instead (m4-qa2-test.mjs and
 * m3-comfort-test.mjs own the "X/Y are inert" assertions). */
const wSaved = await probe(P.WEAPON);
await page.keyboard.press('F6'); /* "save quick" */
await sleep(1000);
console.log('# quicksaved (F6) with weapon=' + wSaved);

/* change weapon with the chunk-2 stick flick (cross-chunk path itself) */
await ctrl('right', 'updateAxes', 'thumbstick', 0, 0.9); /* stick down -> '/' impulse 10 */
await sleep(400);
await ctrl('right', 'updateAxes', 'thumbstick', 0, 0);
await sleep(500);
const wChanged = await probe(P.WEAPON);
check('2d weapon flick changed the weapon post-menu/recenter interactions',
  wChanged !== wSaved, `${wSaved} -> ${wChanged}`);

/* quickload (F9) with the fire trigger HELD across the load */
await ctrl('right', 'updateButtonValue', 'trigger', 1.0);
await sleep(300);
await page.keyboard.press('F9'); /* "load quick" */
/* wait for the load to finish (signon cycles back to 4, player alive) */
await sleep(1200);
await page.waitForFunction(
  () => Module._IN_Weapon_Probe(15) === 4 && Module._IN_Weapon_Probe(14) > 0,
  { timeout: 30000 });
await sleep(800);
check('2d quickload restored the saved weapon', await probe(P.WEAPON) === wSaved,
  `expected ${wSaved}, got ${await probe(P.WEAPON)}`);
await ctrl('right', 'updateButtonValue', 'trigger', 0.0);
await sleep(700);

/* no stuck +attack / haptics: ammo + haptic counter must go quiet */
const s1 = await probe(P.SHELLS), h1 = await probe(P.HAPCNT);
await sleep(1600);
const s2 = await probe(P.SHELLS), h2 = await probe(P.HAPCNT);
check('2d no stuck fire after trigger release post-load (ammo stable)', s1 === s2,
  `${s1} -> ${s2}`);
check('2d no stuck fire haptics post-load (pulse counter stable)', h1 === h2,
  `${h1} -> ${h2}`);

/* aim still live: rotate the controller, gunangles follow */
await page.evaluate(() => {
  const s = Math.sin(Math.PI / 4), c = Math.cos(Math.PI / 4);
  window.__xrdevice.controllers.right.quaternion.set(0, s, 0, c);
  return null;
});
await sleep(600);
check('2d controller aim still drives gunangles after load (yaw ~ 90)',
  Math.abs(await probe(P.GA_Y) - 90) < 5, 'yaw=' + (await probe(P.GA_Y)).toFixed(1));

await page.screenshot({ path: SCREENSHOT });
console.log('# screenshot: ' + SCREENSHOT);

/* clean exit */
await page.evaluate(() => document.getElementById('entervr').click());
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 20000 });
await sleep(500);

const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
console.log('\n# console errors: ' + realErrors.length);
realErrors.forEach((e) => console.log('  ERR: ' + e));

await browser.close();
const ok = fails.length === 0 && realErrors.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + [...fails, ...realErrors].join('; ') + ')'));
process.exit(ok ? 0 : 1);
