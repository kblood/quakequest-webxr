#!/usr/bin/env node
/*
 * m3-weapon-test.mjs — emulated-XR verification of M3 chunk 2 (weapon aim,
 * firing & haptics). Same IWER harness pattern as m3-input-test.mjs, but it
 * starts a REAL game (+skill 0 +map start via argv passthrough) instead of
 * demo playback, drives controller poses/buttons, and asserts on C-side
 * state through the IN_Weapon_Probe export (in_weapon.c):
 *   (a) weapon aim: gunangles follow the controller quaternion, weaponOffset
 *       follows controller-vs-head position, and the engine's gunorg (the
 *       rendered/fire origin) = vieworg + the view.c:975-977 axis-swap of
 *       weaponOffset * vr_worldscale;
 *   (b) firing: trigger press -> +attack -> shell count decreases;
 *   (c) haptics: VR_HapticEvent fired a per-weapon pulse (probe counter) AND
 *       the browser-side hapticActuator.pulse() was invoked (JS spy);
 *   plus: two-handed stabilization, weapon-switch stick flick, laser-sight
 *   cycle, handedness swap, 3DoF/6DoF trackingmode handover on entry/exit.
 *
 * Prereqs:
 *   - build:  WEBOUT=webdist ./build.sh   (this test regenerates
 *             webdist/index.html from ../web/index.html with ?args= support)
 *   - serve:  node web-host/test/serve-dist.mjs 8093
 *   - iwer + puppeteer-core (same env overrides as m3-input-test.mjs)
 *
 * Usage: node web-host/test/m3-weapon-test.mjs [screenshot.png]
 */
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';

const HERE = fileURLToPath(new URL('.', import.meta.url));

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const IWER_PATH = process.env.QQ_IWER_JS
  || 'C:/Users/Caldor/AppData/Local/Temp/claude/C--Devstuff-QuestGames/ad44deaf-c35e-4dbd-b196-b4837498c5e0/scratchpad/node_modules/iwer/build/iwer.min.js';
/* M3 integration: default to the merged build served by `node web/serve.mjs
 * 8090` (was 8093 = this chunk's private worktree webdist server); ?args=
 * argv passthrough is now native to web/index.html, so the webdist
 * regeneration step below is gone. */
const URL_BASE = process.env.QQ_URL || 'http://localhost:8090/';
const TEST_URL = URL_BASE + '?autostart=1&inputdebug=1&args=' +
  encodeURIComponent('+skill 0 +map start');
const SCREENSHOT = process.argv[2] || 'm3-weapon-emulated.png';

const CHROME = [process.env.QQ_CHROME,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']
  .filter(Boolean).find(existsSync);
if (!CHROME) { console.error('no chrome found (set QQ_CHROME)'); process.exit(2); }
if (!existsSync(IWER_PATH)) { console.error('iwer not found (set QQ_IWER_JS): ' + IWER_PATH); process.exit(2); }

// (the webdist/index.html regeneration step lived here pre-merge; ?args=
// argv passthrough was upstreamed to web/index.html at M3 integration)

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
// IN_Weapon_Probe indices (in_weapon.c)
const P = { GA_P: 0, GA_Y: 1, GA_R: 2, WO_X: 3, WO_Y: 4, WO_Z: 5,
  GUN_X: 6, GUN_Y: 7, GUN_Z: 8, VIEW_X: 9, VIEW_Y: 10, VIEW_Z: 11,
  WEAPON: 12, SHELLS: 13, HEALTH: 14, SIGNON: 15, STAB: 16,
  HAPCNT: 17, HAPLVL: 18, TRACK: 19, AIMACT: 20, LASER: 21, WSCALE: 22,
  HAPCHAN: 23, RIGHTH: 24, HMD_X: 25, HMD_Y: 26, HMD_Z: 27 };
const probe = (i) => page.evaluate((i) => Module._IN_Weapon_Probe(i), i);
const probes = (l) => page.evaluate((l) => l.map((i) => Module._IN_Weapon_Probe(i)), l);
const setCvar = (i, v) => page.evaluate((i, v) => Module._IN_Weapon_TestSet(i, v), i, v);
const near = (a, b, tol) => Math.abs(a - b) <= tol;
const IT_SHOTGUN = 1, IT_AXE = 4096;

console.log("# loading " + TEST_URL);
await page.goto(TEST_URL, { waitUntil: 'load' });
await page.waitForFunction(
  () => document.getElementById('start')?.textContent === 'Running', { timeout: 45000 });

// wait for the actual game (not a demo): signon complete + player alive
await page.waitForFunction(
  () => Module._IN_Weapon_Probe && Module._IN_Weapon_Probe(15) === 4 && Module._IN_Weapon_Probe(14) > 0,
  { timeout: 45000 });
console.log('# map start is up (signon=4, health=' + await probe(P.HEALTH) + ')');
check('starts with shotgun active', await probe(P.WEAPON) === IT_SHOTGUN, 'weapon=' + await probe(P.WEAPON));
check('flatscreen trackingmode is 3DoF', await probe(P.TRACK) === 0);

// ---- enter VR ----
await page.waitForFunction(() => !document.getElementById('entervr')?.disabled);
await page.click('#entervr');
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Exit VR', { timeout: 20000 });
await sleep(1500);
console.log('# immersive session active');
check('VR trackingmode is 6DoF', await probe(P.TRACK) === 1);

// spy on the browser-side haptic actuators (verification c, JS half)
await page.evaluate(() => {
  window.__pulses = [];
  const s = Module.webxr_session;
  for (const src of s.inputSources) {
    const act = src.gamepad && src.gamepad.hapticActuators && src.gamepad.hapticActuators[0];
    if (act && !act.__spied) {
      act.__spied = true;
      const orig = act.pulse.bind(act);
      act.pulse = (i, d) => { window.__pulses.push({ hand: src.handedness, i, d }); return orig(i, d); };
    }
  }
  return null;
});

// ---- (a) aim: identity orientation, known position ----
await page.evaluate(() => {
  const dev = window.__xrdevice;
  dev.controllers.right.position.set(0.35, 1.1, -0.45);
  dev.controllers.right.quaternion.set(0, 0, 0, 1);
  dev.controllers.left.position.set(-0.6, 1.1, -0.2); // far from right hand
});
await sleep(700);

check('controller aim is active', await probe(P.AIMACT) === 1);
{
  const [gp, gy, wox, woy, woz, hx, hy, hz] = await probes(
    [P.GA_P, P.GA_Y, P.WO_X, P.WO_Y, P.WO_Z, P.HMD_X, P.HMD_Y, P.HMD_Z]);
  // identity quat -> yaw 0; vr_weaponpitchadjust(-20) bakes a fixed pitch trim
  check('gunangles yaw ~ 0 (identity quat)', near(gy, 0, 3), 'yaw=' + gy.toFixed(2));
  check('gunangles pitch = weapon pitch trim (|20|)', near(Math.abs(gp), 20, 3), 'pitch=' + gp.toFixed(2));
  // weaponOffset = controller - head (yawOffset ~ 0 -> rotation is identity)
  check('weaponOffset.x = ctrl-head', near(wox, 0.35 - hx, 0.03), `wox=${wox.toFixed(3)} exp=${(0.35 - hx).toFixed(3)}`);
  check('weaponOffset.y = ctrl-head', near(woy, 1.1 - hy, 0.03), `woy=${woy.toFixed(3)} exp=${(1.1 - hy).toFixed(3)}`);
  check('weaponOffset.z = ctrl-head', near(woz, -0.45 - hz, 0.03), `woz=${woz.toFixed(3)} exp=${(-0.45 - hz).toFixed(3)}`);
}
{
  // gun origin followed the hand: gunorg - vieworg must equal the
  // view.c:975-977 axis swap of weaponOffset * vr_worldscale
  const [gx, gyy, gz, vx, vy, vz, wox, woy, woz, ws] = await probes(
    [P.GUN_X, P.GUN_Y, P.GUN_Z, P.VIEW_X, P.VIEW_Y, P.VIEW_Z, P.WO_X, P.WO_Y, P.WO_Z, P.WSCALE]);
  const tol = 0.5; // quake units (values sampled across frames)
  check('gunorg.x = vieworg.x - wo.z*worldscale', near(gx - vx, -woz * ws, tol),
    `d=${(gx - vx).toFixed(2)} exp=${(-woz * ws).toFixed(2)}`);
  check('gunorg.y = vieworg.y - wo.x*worldscale', near(gyy - vy, -wox * ws, tol),
    `d=${(gyy - vy).toFixed(2)} exp=${(-wox * ws).toFixed(2)}`);
  check('gunorg.z = vieworg.z + wo.y*worldscale', near(gz - vz, woy * ws, tol),
    `d=${(gz - vz).toFixed(2)} exp=${(woy * ws).toFixed(2)}`);
}

// rotate the controller 90° left (about +Y): aim must follow to yaw ~ 90
await page.evaluate(() => {
  const s = Math.sin(Math.PI / 4), c = Math.cos(Math.PI / 4);
  window.__xrdevice.controllers.right.quaternion.set(0, s, 0, c);
});
await sleep(500);
check('gunangles yaw ~ 90 after +90° controller turn',
  near(await probe(P.GA_Y), 90, 4), 'yaw=' + (await probe(P.GA_Y)).toFixed(2));
await page.evaluate(() => { window.__xrdevice.controllers.right.quaternion.set(0, 0, 0, 1); });
await sleep(400);

// ---- two-handed stabilization ----
check('not stabilised with hands apart', await probe(P.STAB) === 0);
await page.evaluate(() => {
  const dev = window.__xrdevice;
  // off-hand 0.4m straight ahead of the dominant hand, squeeze held
  dev.controllers.left.position.set(0.35, 1.1, -0.85);
  dev.controllers.left.updateButtonValue('squeeze', 1.0);
});
await sleep(500);
check('stabilised (grip held, hands close, not axe)', await probe(P.STAB) === 1);
check('stabilised aim: pitch from hand-to-hand vector ~ 0 (not the -20 trim)',
  near(await probe(P.GA_P), 0, 3), 'pitch=' + (await probe(P.GA_P)).toFixed(2));
await page.evaluate(() => {
  // move off-hand so the two-hand vector points 45° left
  window.__xrdevice.controllers.left.position.set(0.07, 1.1, -0.73);
});
await sleep(500);
check('stabilised aim follows two-hand vector (yaw ~ 45)',
  near(await probe(P.GA_Y), 45, 4), 'yaw=' + (await probe(P.GA_Y)).toFixed(2));
await page.evaluate(() => {
  window.__xrdevice.controllers.left.updateButtonValue('squeeze', 0);
  window.__xrdevice.controllers.left.position.set(-0.6, 1.1, -0.2);
});
await sleep(400);
check('stabilisation released with grip', await probe(P.STAB) === 0);

// ---- (b)+(c) fire: trigger -> +attack -> ammo down; haptic pulse out ----
const shells0 = await probe(P.SHELLS);
const hap0 = await probe(P.HAPCNT);
await page.evaluate(() => { window.__xrdevice.controllers.right.updateButtonValue('trigger', 1.0); });
await sleep(900);
await page.evaluate(() => { window.__xrdevice.controllers.right.updateButtonValue('trigger', 0); });
await sleep(400);
const shells1 = await probe(P.SHELLS);
const hap1 = await probe(P.HAPCNT);
check('trigger fired the shotgun (shells decreased)', shells1 < shells0, `${shells0} -> ${shells1}`);
check('VR_HapticEvent pulsed on fire (C-side)', hap1 > hap0, `count ${hap0} -> ${hap1}`);
check('haptic used shotgun table level 0.7', near(await probe(P.HAPLVL), 0.7, 0.01));
check('haptic channel = dominant right (2)', await probe(P.HAPCHAN) === 2);
const pulses = await page.evaluate(() => window.__pulses);
check('browser hapticActuator.pulse() observed (right hand)',
  pulses.some((p) => p.hand === 'right' && p.i > 0), JSON.stringify(pulses.slice(0, 3)));

// ---- weapon-switch flick (right stick y) ----
await page.evaluate(() => { window.__xrdevice.controllers.right.updateAxes('thumbstick', 0, 0.9); }); // xr-standard +y = stick DOWN -> OpenXR y=-0.9 -> '/' = impulse 10 (prev)
await sleep(400);
await page.evaluate(() => { window.__xrdevice.controllers.right.updateAxes('thumbstick', 0, 0); });
await sleep(500);
check('stick-down flick switched to prev weapon (axe)', await probe(P.WEAPON) === IT_AXE,
  'weapon=' + await probe(P.WEAPON));
await page.evaluate(() => { window.__xrdevice.controllers.right.updateAxes('thumbstick', 0, -0.9); }); // stick UP -> '#' = impulse 12 (next)
await sleep(400);
await page.evaluate(() => { window.__xrdevice.controllers.right.updateAxes('thumbstick', 0, 0); });
await sleep(500);
check('stick-up flick switched back (shotgun)', await probe(P.WEAPON) === IT_SHOTGUN,
  'weapon=' + await probe(P.WEAPON));

// ---- laser-sight cycle on dominant thumbstick click ----
const laser0 = await probe(P.LASER);
await page.evaluate(() => { window.__xrdevice.controllers.right.updateButtonValue('thumbstick', 1.0); });
await sleep(300);
await page.evaluate(() => { window.__xrdevice.controllers.right.updateButtonValue('thumbstick', 0); });
await sleep(300);
check('thumbstick click cycled r_lasersight', await probe(P.LASER) === (laser0 + 1) % 3,
  `${laser0} -> ${await probe(P.LASER)}`);

// ---- handedness: cl_righthanded 0 makes the LEFT hand aim + fire ----
await setCvar(P.RIGHTH, 0);
await sleep(400);
check('cl_righthanded 0 applied', await probe(P.RIGHTH) === 0);
await page.evaluate(() => {
  const dev = window.__xrdevice;
  dev.controllers.left.position.set(-0.3, 1.2, -0.4);
  const s = Math.sin(-Math.PI / 8), c = Math.cos(-Math.PI / 8); // -45°/2*2 = -45°? no: 2*22.5 = -45
  dev.controllers.left.quaternion.set(0, s, 0, c); // -45° about Y
});
await sleep(500);
check('left-handed: gunangles follow LEFT controller (yaw ~ -45)',
  near(await probe(P.GA_Y), -45, 4), 'yaw=' + (await probe(P.GA_Y)).toFixed(2));
const shells2 = await probe(P.SHELLS);
await page.evaluate(() => { window.__xrdevice.controllers.left.updateButtonValue('trigger', 1.0); });
await sleep(900);
await page.evaluate(() => { window.__xrdevice.controllers.left.updateButtonValue('trigger', 0); });
await sleep(400);
check('left-handed: LEFT trigger fires', await probe(P.SHELLS) < shells2,
  `${shells2} -> ${await probe(P.SHELLS)}`);
check('left-handed haptic channel = left (1)', await probe(P.HAPCHAN) === 1);
await setCvar(P.RIGHTH, 1);
await sleep(300);

await page.screenshot({ path: SCREENSHOT });
console.log('# screenshot: ' + SCREENSHOT);

// ---- exit VR: trackingmode restored for flatscreen ----
await page.evaluate(() => document.getElementById('entervr').click());
await page.waitForFunction(
  () => document.getElementById('entervr')?.textContent === 'Enter VR', { timeout: 20000 });
await sleep(600);
check('flatscreen trackingmode restored (3DoF)', await probe(P.TRACK) === 0);
check('aim inactive outside session', await probe(P.AIMACT) === 0);

const realErrors = errors.filter((e) => !e.includes('glCopyTexSubImage2D')); // known cosmetic (PORT_NOTES)
console.log('\n# console errors: ' + realErrors.length);
realErrors.forEach((e) => console.log('  ERR: ' + e));

await browser.close();
const ok = fails.length === 0 && realErrors.length === 0;
console.log('\n# verdict: ' + (ok ? 'OK' : 'FAIL (' + [...fails, ...realErrors].join('; ') + ')'));
process.exit(ok ? 0 : 1);
