#!/usr/bin/env node
/*
 * m4-qa2-test.mjs — headset-QA round 2 regression suite.
 *
 * Covers the three on-device findings (PORT_NOTES.md "Headset QA round 2"):
 *
 *  (T1) REALISTIC GAMEPAD SHAPE: every check below runs against gamepads
 *       shimmed to the EXACT real Quest Touch shape — a 7-entry buttons
 *       array including the placeholder touchpad slot at [2] (thumbstick
 *       click at [3], X/A at [4], Y/B at [5], thumbrest at [6]) and a
 *       4-entry axes array with NUMERIC zeros at [0,1] and the thumbstick
 *       on [2,3] — driven by direct array mutation, fully independent of
 *       IWER's controller API. If the marshaling (lib/webxr PATCH #12) or
 *       webxr_input.c's index constants ever drift from the device layout,
 *       this suite catches it even if the emulator would mask it.
 *  (T2) THE SHIFT-ESCAPE TRAP (root cause of "menu cannot be opened
 *       on-device"): short-click the off-hand thumbstick WITH THE RUN
 *       TRIGGER HELD. Pre-fix, run was a K_SHIFT key event and
 *       keys.c:1815-1839 turned the synthesized Escape into a CONSOLE
 *       toggle (menu never opened — reproduces the on-device failure).
 *       Post-fix (run = direct +speed, no keydown[K_SHIFT]) the menu opens.
 *  (T3) LEFT X = PRIMARY MENU TOGGLE: opens in-game, closes while up —
 *       including with the run trigger held.
 *  (T4) RIGHT B = DUCK (hold): +movedown engages ('c' key -> bind ->
 *       kbutton chain, WebXRLoco_Probe), the artificial-crouch eye offset
 *       ramps to 0.45 m and the reported controller pose Y drops with it;
 *       release restores everything.
 *  (T5) X/Y MUST NOT SAVE/LOAD: with a known console-made quicksave, Y
 *       must not restore it and X must not overwrite it (X opens the menu
 *       instead).
 *
 * Prereqs: build served on 8090 (node web/serve.mjs 8090). Env overrides:
 * see m3-input-test.mjs header. Usage: node m4-qa2-test.mjs [screenshot.png]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
const URL = process.env.QQ_URL || 'http://localhost:8090/?autostart=1&inputdebug=1&args='
  + encodeURIComponent('+skill 0 +map start');
const SCREENSHOT = process.argv[2] || 'm4-qa2-test.png';

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
  console.log(`  [${cond ? 'PASS' : 'FAIL'}] ${name}${detail !== undefined ? ' — ' + detail : ''}`);
  if (!cond) fails.push(name);
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const overlay = () => page.evaluate(() =>
  (document.getElementById('qq-inputdebug') || {}).textContent || '');
const menuSt = async () => {
  const v = await page.evaluate(() => Module._VRMenuQuad_DebugState());
  return { raw: v, quad: v & 1, bigScreen: (v >> 4) & 0xf, mState: (v >> 16) & 0xff };
};
/* defensive probe: returns -2 when the export is missing (pre-fix builds)
 * so a pre-fix run reports FAILs instead of crashing the harness */
const locoProbe = (i) => page.evaluate((i) =>
  (typeof Module._WebXRLoco_Probe === 'function') ? Module._WebXRLoco_Probe(i) : -2, i);
const weapon = () => page.evaluate(() => Module._WebXRComfort_DebugGetActiveWeapon());
async function typeConsole(cmd) {
  await page.keyboard.press('Backquote');
  await sleep(150);
  await page.keyboard.type(cmd);
  await page.keyboard.press('Enter');
  await sleep(150);
  await page.keyboard.press('Backquote');
  await sleep(300);
}

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
console.log('# immersive session active');

/* ============ T1: install the device-shaped gamepad shim ============ */
await page.evaluate(() => {
  // Real Quest Touch xr-standard capture shape (webxr-input-profiles /
  // felixtrz.github.io/webxr-device-config): 7 buttons incl. the
  // placeholder touchpad slot, 4 axes with numeric 0s in the unused pair.
  const mkPad = (orig) => {
    const buttons = Array.from({ length: 7 }, () =>
      ({ pressed: false, touched: false, value: 0 }));
    const axes = [0, 0, 0, 0];
    return {
      pad: {
        id: '', index: -1, connected: true, mapping: 'xr-standard',
        buttons, axes,
        hapticActuators: (orig && orig.hapticActuators) || [],
      },
      buttons, axes,
    };
  };
  window.__pads = {};
  for (const src of Module.webxr_session.inputSources) {
    const shim = mkPad(src.gamepad);
    Object.defineProperty(src, 'gamepad', { value: shim.pad, configurable: true });
    window.__pads[src.handedness] = shim;
  }
  window.__press = (hand, i, v) => {
    const b = window.__pads[hand].buttons[i];
    b.value = v; b.pressed = v > 0.5; b.touched = v > 0;
  };
  window.__stick = (hand, x, y) => {
    window.__pads[hand].axes[2] = x;
    window.__pads[hand].axes[3] = y;
  };
  return Object.keys(window.__pads).join(',');
});
const press = (hand, i, v) => page.evaluate(([h, i, v]) => window.__press(h, i, v), [hand, i, v]);
const stick = (hand, x, y) => page.evaluate(([h, x, y]) => window.__stick(h, x, y), [hand, x, y]);
await sleep(400);

let txt = await overlay();
check('T1 shim live: both hands report the 7-button/4-axis device shape',
  (txt.match(/nb=7 na=4/g) || []).length === 2, (txt.match(/nb=\d na=\d/g) || []).join(' '));
check('T1 no buttons stuck at rest', /btn=00000000/.test(txt));

/* thumbstick on axes[2,3] (numeric [0,1] zeros must be ignored) */
await stick('right', 0.5, -0.8);
await sleep(400);
txt = await overlay();
check('T1 right stick x=0.50 read from axes[2]', /^R:[\s\S]*?stick\(\s*0\.50/m.test(txt));
check('T1 right stick y=+0.80 (sign-flipped) read from axes[3]',
  /^R:[\s\S]*?stick\(\s*0\.50\s+0\.80\)/m.test(txt));
await stick('right', 0, 0);
await sleep(300);

/* button indices: [3] stick click, [4] X, [5] B through the real layout */
await press('left', 3, 1);
await press('left', 4, 1);
await sleep(400);
txt = await overlay();
{
  const lBtn = (txt.match(/^L:[\s\S]*?btn=([0-9a-f]{8})/m) || [])[1];
  check('T1 left stick-click at buttons[3] -> LThumb|Joystick bits',
    lBtn && ((parseInt(lBtn, 16) & 0x80000400) >>> 0) === 0x80000400, 'btn=' + lBtn);
  check('T1 left X at buttons[4] -> xrButton_X bit',
    lBtn && (parseInt(lBtn, 16) & 0x100) !== 0, 'btn=' + lBtn);
}
await press('left', 3, 0);
await press('left', 4, 0);
await sleep(600);
/* the X press above may have opened the menu (that IS its job now) —
 * normalize back to in-game before the functional tests */
let st = await menuSt();
if (st.mState !== 0 || st.bigScreen !== 0) {
  await press('left', 4, 1); await sleep(300);
  await press('left', 4, 0); await sleep(500);
  st = await menuSt();
}
check('T1 back in-game after index checks', st.mState === 0 && st.bigScreen === 0,
  JSON.stringify(st));

/* ============ T2: the shift-escape trap (root-cause regression) ====== */
/* hold the off-hand RUN trigger — buttons[0], analog 1.0 — then a short
 * (<600 ms) stick click. Pre-fix: console toggles, menu stays shut. */
await press('left', 0, 1.0);
await sleep(400);
check('T2 run engaged while trigger held (+speed active)', (await locoProbe(3)) === 1,
  'probe=' + await locoProbe(3));
await press('left', 3, 1);
await sleep(220);
await press('left', 3, 0);
await sleep(500);
st = await menuSt();
check('T2 stick-click WITH RUN HELD opens the MENU (not the console) — shift-escape trap',
  st.mState === 1 && st.quad === 1, JSON.stringify(st));
/* close it the same way (trigger still held) */
await press('left', 3, 1);
await sleep(220);
await press('left', 3, 0);
await sleep(500);
st = await menuSt();
check('T2 second click closes it again (still trigger-held)',
  st.mState === 0 && st.bigScreen === 0, JSON.stringify(st));
await press('left', 0, 0);
await sleep(400);
check('T2 run released (+speed off)', (await locoProbe(3)) === 0);

/* ============ T3: left X = primary menu toggle ======================= */
await press('left', 4, 1);
await sleep(300);
await press('left', 4, 0);
await sleep(500);
st = await menuSt();
check('T3 X opens the menu in-game', st.mState === 1 && st.quad === 1, JSON.stringify(st));
await press('left', 4, 1);
await sleep(300);
await press('left', 4, 0);
await sleep(500);
st = await menuSt();
check('T3 X closes the menu again', st.mState === 0 && st.bigScreen === 0, JSON.stringify(st));
/* and with run held (belt & suspenders on the same trap class) */
await press('left', 0, 1.0);
await sleep(300);
await press('left', 4, 1);
await sleep(300);
await press('left', 4, 0);
await sleep(500);
st = await menuSt();
check('T3 X opens the menu even with the run trigger held', st.mState === 1,
  JSON.stringify(st));
await press('left', 4, 1);
await sleep(300);
await press('left', 4, 0);
await press('left', 0, 0);
await sleep(500);

/* ============ T4: right A = duck (hold) ==============================
 * (round-3 swap: duck moved B -> A, jump A -> B, on user request)      */
/* park the right controller at a known height so the pose drop is visible */
await page.evaluate(() => {
  window.__xrdevice.controllers.right.position.set(0.3, 1.2, -0.4);
  return null;
});
await sleep(400);
const aimYOf = (t) => {
  const m = t.match(/^R:[\s\S]*?aim.?\(\s*(-?[\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)\)/m);
  return m ? +m[2] : null;
};
const aimYBefore = aimYOf(await overlay());
check('T4 duck inert before press', (await locoProbe(0)) === 0 && (await locoProbe(1)) === 0);
await press('right', 4, 1); /* A = buttons[4] */
await sleep(500);
check('T4 A hold engages +movedown (key \'c\' -> bind -> kbutton chain)',
  (await locoProbe(1)) === 1, 'probe=' + await locoProbe(1));
{
  const off = await locoProbe(0);
  check('T4 eye offset ramped to 0.45 m', Math.abs(off - 0.45) < 0.01, 'offset=' + off);
}
{
  const aimYDucked = aimYOf(await overlay());
  check('T4 controller pose Y ducks with the head (gun follows the crouch)',
    aimYBefore !== null && aimYDucked !== null && Math.abs((aimYBefore - aimYDucked) - 0.45) < 0.02,
    `aimY ${aimYBefore} -> ${aimYDucked}`);
}
await press('right', 4, 0);
await sleep(500);
check('T4 release lets go of +movedown', (await locoProbe(1)) === 0);
check('T4 eye offset back to 0', (await locoProbe(0)) === 0, 'offset=' + await locoProbe(0));

/* ============ T5: X/Y must not save/load ============================= */
const w0 = await weapon();
await typeConsole('save quick');
await sleep(500);
await typeConsole('impulse 12');
await sleep(400);
const wChanged = await weapon();
check('T5 setup: weapon changed after console quicksave', wChanged !== w0,
  `${w0} -> ${wChanged}`);
/* Y (buttons[5] left) must NOT quickload */
await press('left', 5, 1);
await sleep(800);
await press('left', 5, 0);
await sleep(800);
check('T5 left Y does NOT quickload (weapon unchanged)', (await weapon()) === wChanged,
  `expected ${wChanged}, got ${await weapon()}`);
/* X must NOT quicksave (it opens the menu). If it saved, the slot would
 * now hold wChanged and the load below would not restore w0. */
await press('left', 4, 1);
await sleep(300);
await press('left', 4, 0);
await sleep(500);
st = await menuSt();
check('T5 left X opens the menu (not a quicksave)', st.mState === 1, JSON.stringify(st));
await press('left', 4, 1);
await sleep(300);
await press('left', 4, 0);
await sleep(500);
await typeConsole('load quick');
await sleep(1200);
await page.waitForFunction(
  () => Module._IN_Weapon_Probe(15) === 4 && Module._IN_Weapon_Probe(14) > 0,
  { timeout: 30000 });
await sleep(600);
check('T5 console `load quick` restores the ORIGINAL save (X did not overwrite it)',
  (await weapon()) === w0, `expected ${w0}, got ${await weapon()}`);

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
