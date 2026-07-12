#!/usr/bin/env node
/*
 * m4-trackingmode-test.mjs — cl_trackingmode config-stomp fix (M4;
 * reports/08b open issue 3).
 *
 * The bug: cl_trackingmode is CVAR_SAVE, but the mode-switch code forced it
 * per-context (0 on flatscreen boot / 1 on VR entry / 0 on VR exit), so
 * whatever value happened to be forced at the 10s persist tick was what
 * config.cfg recorded — the user's 3DoF/6DoF choice never survived a reload.
 *
 * The fix (in_weapon.c): the USER PREFERENCE is latched from config.cfg at
 * init and re-latched whenever the cvar changes through anything that is not
 * the module's own forced write; VR entry applies the preference (not a
 * hardcoded 1); saves go through IN_Weapon_SaveConfigPreservingTrackingMode
 * which swaps the preference in around Host_SaveConfig.
 *
 * Asserted here, with IWER emulated XR (same harness as m3-weapon-test):
 *   1. flatscreen boot: runtime forced 0, preference = 1 (cvar default)
 *   2. persist while flatscreen-forced -> config.cfg says 1, NOT 0  [the bug]
 *   3. VR entry applies the preference (1) ... VR exit restores 0
 *   4. user picks 3DoF in-session (TestSet, same path as the VR options
 *      menu toggle) -> preference re-latches to 0
 *   5. persist -> config.cfg says 0; reload -> preference restored from
 *      config.cfg; VR entry now applies 0, NOT a forced 1        [the fix]
 *
 * Prereqs: build served locally (node web/serve.mjs 8090); iwer +
 * puppeteer-core (env QQ_URL / QQ_CHROME / QQ_PUPPETEER / QQ_IWER_JS).
 * Usage: node m4-trackingmode-test.mjs [screenshot.png]
 */
import { readFileSync, existsSync } from 'node:fs';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
const URL_BASE = process.env.QQ_URL || 'http://localhost:8090/';
const TEST_URL = URL_BASE + '?autostart=1&args=' + encodeURIComponent('+skill 0 +map start');
const SCREENSHOT = process.argv[2] || 'm4-trackingmode-test.png';

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

// evaluateOnNewDocument persists across the mid-test reload
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
// IN_Weapon_Probe indices (in_weapon.c): 19 = cl_trackingmode runtime value,
// 28 = s_userTrackingMode (the saved preference)
const TRACK = 19, PREF = 28;
const probe = (i) => page.evaluate((i) => Module._IN_Weapon_Probe(i), i);
const setTrack = (v) => page.evaluate((v) => Module._IN_Weapon_TestSet(19, v), v);
const persistAndReadConfig = async () => {
  await page.evaluate(() => Module._WebHost_PersistNow());
  const cfg = await page.evaluate(() =>
    new TextDecoder().decode(Module.FS.readFile('/quake_user/id1/config.cfg')));
  // fork's Cvar_WriteVariables quotes the name: "cl_trackingmode" "0"
  const m = cfg.match(/^"?cl_trackingmode"? "([^"]*)"/m);
  // DP's Cvar_WriteVariables omits CVAR_SAVE cvars still at their default
  // string — cl_trackingmode's default is "1", so no line == saved as 1.
  return m ? m[1] : '1';
};

async function bootAndWait() {
  await page.goto(TEST_URL, { waitUntil: 'load' });
  await page.waitForFunction(
    () => document.getElementById('start')?.textContent === 'Running', { timeout: 45000 });
  await page.waitForFunction(
    () => Module._IN_Weapon_Probe && Module._IN_Weapon_Probe(15) === 4
       && Module._IN_Weapon_Probe(14) > 0, { timeout: 45000 });
}
async function enterVR() {
  await page.waitForFunction(() => !document.getElementById('entervr')?.disabled);
  await page.click('#entervr');
  await page.waitForFunction(
    () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 20000 });
  await sleep(1200); // a few XR frames so IN_Weapon_Update has run
}
async function exitVR() {
  // evaluate-click: page.click's synthesized mouse event doesn't reach the
  // button while the emulated immersive session is presenting
  await page.evaluate(() => document.getElementById('entervr').click());
  await page.waitForFunction(
    () => document.getElementById('entervr')?.textContent !== 'Exit VR', { timeout: 20000 });
  await sleep(600);
}

console.log('# M4 cl_trackingmode stomp-fix test: ' + TEST_URL);
await bootAndWait();

// (1) flatscreen boot: runtime forced 3DoF, preference = default 1
check('flatscreen runtime forced to 0 (3DoF)', await probe(TRACK) === 0);
check('preference latched as 1 (cvar default)', await probe(PREF) === 1);

// (2) THE BUG: persist while the flatscreen force is active
const saved1 = await persistAndReadConfig();
check('config.cfg saves the PREFERENCE (1), not the forced 0', saved1 === '1',
  'cl_trackingmode "' + saved1 + '"');
check('runtime still 0 after the save swap-around', await probe(TRACK) === 0);

// (3) VR entry applies the preference; exit restores flatscreen 0
await enterVR();
check('VR entry applies preference (6DoF)', await probe(TRACK) === 1);

// (4) user toggles to 3DoF in-session (same path as the VR options menu)
await setTrack(0);
await sleep(600); // IN_Weapon_TrackingModeTick runs each XR frame
check('in-session user change re-latches preference to 0', await probe(PREF) === 0);
await exitVR();
check('VR exit restores flatscreen 0', await probe(TRACK) === 0);
check('preference survives session end (still 0)', await probe(PREF) === 0);

// (5) THE FIX end-to-end: persist, reload, preference restored, VR honors it
const saved0 = await persistAndReadConfig();
check('config.cfg now saves preference 0', saved0 === '0',
  'cl_trackingmode "' + saved0 + '"');
await bootAndWait();
check('reload: preference restored from config.cfg', await probe(PREF) === 0);
check('reload: flatscreen runtime forced 0', await probe(TRACK) === 0);
await enterVR();
check('VR entry after reload applies SAVED 0 (not forced 1)', await probe(TRACK) === 0);
await exitVR();

// flatscreen console change is also a user change (ticked in web_frame)
await setTrack(1);
await sleep(400); // flatscreen rAF frames tick IN_Weapon_TrackingModeTick
check('flatscreen user change re-latches preference to 1', await probe(PREF) === 1);
const saved1b = await persistAndReadConfig();
check('...and persists as 1', saved1b === '1', 'cl_trackingmode "' + saved1b + '"');

check('no console errors', errors.length === 0, errors.slice(0, 3).join(' | '));
await page.screenshot({ path: SCREENSHOT });
await browser.close();

console.log(`\n# verdict: ${fails.length === 0 ? 'OK' : 'FAIL'} (${fails.length} fail)`);
process.exit(fails.length === 0 ? 0 : 1);
