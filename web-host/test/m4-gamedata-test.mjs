#!/usr/bin/env node
/*
 * m4-gamedata-test.mjs — full-game data drop-in mechanism (M4).
 *
 * Exercises web/index.html's pak file-picker end to end with a SYNTHETIC
 * registered-marker pak (a minimal valid PACK archive containing
 * gfx/pop.lmp — the file FS_Rescan checks to set the `registered` cvar):
 *
 *   1. upload pak1.pak BEFORE engine start -> boot prints
 *      "Playing registered version." (FS_Init picked it up from
 *      /quake_user/id1, the IDBFS userdir)
 *   2. Remove button while running -> _WebHost_RescanFS ->
 *      "Playing shareware version." (live search-path rebuild)
 *   3. re-upload while running -> rescan -> registered again
 *   4. reload the page -> pak persisted in IndexedDB -> boot registered
 *   5. cleanup: Remove + reload -> shareware again
 *
 * Optionally, set QQ_KEX_PAK to a real pak (e.g. the GOG "Quake Enhanced"
 * KEX id1/pak0.pak) to test engine compatibility with that file: it is
 * uploaded, the engine rescans and a new game is started; the test reports
 * console errors/progs behavior instead of asserting (compat recon).
 * The file is read from its original location only — nothing is copied.
 *
 * Prereqs: build served locally (node web/serve.mjs 8090).
 * Env: QQ_URL (default http://localhost:8090/), QQ_PUPPETEER, QQ_CHROME.
 * Usage: node m4-gamedata-test.mjs [screenshot.png]
 */
import { readFileSync, writeFileSync, existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const BASEURL = (process.env.QQ_URL || 'http://localhost:8090/').replace(/\?.*$/, '');
const SCREENSHOT = process.argv[2] || 'm4-gamedata-test.png';
const KEX_PAK = process.env.QQ_KEX_PAK || '';

const CHROME = [process.env.QQ_CHROME,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']
  .filter(Boolean).find(existsSync);
if (!CHROME) { console.error('no chrome found (set QQ_CHROME)'); process.exit(2); }

/* ---- build the synthetic pak: PACK header + one file (gfx/pop.lmp) ---- */
function buildPak(entries) {
  // entries: [{name, data:Uint8Array}]
  let fileofs = 12;
  const blobs = [];
  const dir = Buffer.alloc(64 * entries.length);
  entries.forEach((e, i) => {
    dir.write(e.name, i * 64, 55, 'latin1');
    dir.writeInt32LE(fileofs, i * 64 + 56);
    dir.writeInt32LE(e.data.length, i * 64 + 60);
    blobs.push(Buffer.from(e.data));
    fileofs += e.data.length;
  });
  const head = Buffer.alloc(12);
  head.write('PACK', 0, 'latin1');
  head.writeInt32LE(fileofs, 4);       // dir offset (after header+files)
  head.writeInt32LE(dir.length, 8);    // dir size
  return Buffer.concat([head, ...blobs, dir]);
}
const tmp = mkdtempSync(join(tmpdir(), 'qq-m4-'));
const PAK_PATH = join(tmp, 'pak1.pak');
writeFileSync(PAK_PATH, buildPak([
  { name: 'gfx/pop.lmp', data: new TextEncoder().encode('m4 synthetic registered marker') },
]));

/* ---- helpers ---- */
let passCount = 0, failCount = 0;
function check(name, ok, detail = '') {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? ' — ' + detail : ''}`);
  ok ? passCount++ : failCount++;
}

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: true, args: ['--no-sandbox'],
});

let page, logs, errors;
async function openPage(url) {
  if (page) await page.close();
  page = await browser.newPage();
  page.setDefaultTimeout(45000);
  logs = []; errors = [];
  page.on('console', (m) => {
    logs.push(m.text());
    if (m.type() === 'error') errors.push(m.text());
  });
  page.on('pageerror', (e) => errors.push('PAGEERROR: ' + e.message));
  await page.goto(url, { waitUntil: 'networkidle2' });
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function waitForLog(rx, timeoutMs = 20000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (logs.some((l) => rx.test(l))) return true;
    await sleep(250);
  }
  return false;
}
async function uploadPak(path) {
  const input = await page.$('#pak-input');
  await input.uploadFile(path);
  const t0 = Date.now();
  while (Date.now() - t0 < 120000) {
    const s = await page.$eval('#pak-status', (el) => el.textContent);
    if (/stored locally/.test(s)) return s;
    if (/failed/.test(s)) throw new Error('pak store failed: ' + s);
    await sleep(250);
  }
  throw new Error('pak upload timed out');
}
const startEngine = () => page.click('#start');
const localPaks = () => page.evaluate(() =>
  { try { return Module.FS.readdir('/quake_user/id1').filter(n => /\.(pak|pk3)$/i.test(n)); }
    catch (e) { return []; } });

/* =================== the test =================== */
console.log('# M4 game-data drop-in test (synthetic pak = ' + PAK_PATH + ')');

// -- fresh state: make sure no leftover paks from an earlier run
await openPage(BASEURL);
await page.waitForFunction(() => !document.getElementById('pak-input').disabled);
const launcher = await page.evaluate(() => ({
  data: Array.from(document.querySelectorAll('input[name="game-data"]')).map((e) => e.value),
  modes: Array.from(document.querySelectorAll('input[name="launch-mode"]')).map((e) => e.value),
  folderPicker: !!document.getElementById('folder-input')?.webkitdirectory,
}));
check('launcher offers bundled and local data',
  launcher.data.join(',') === 'shareware,local', JSON.stringify(launcher));
check('launcher offers flat/WASM and WebXR modes',
  launcher.modes.join(',') === 'flat,webxr', JSON.stringify(launcher));
check('launcher exposes a Quake folder picker', launcher.folderPicker, JSON.stringify(launcher));
const filteredFolderFiles = await page.evaluate(() => filesFromId1Folder([
  { name: 'pak0.pak', webkitRelativePath: 'Quake/id1/pak0.pak' },
  { name: 'pak0.pak', webkitRelativePath: 'Quake/hipnotic/pak0.pak' },
  { name: 'readme.txt', webkitRelativePath: 'Quake/id1/readme.txt' },
]).map((file) => file.webkitRelativePath));
check('folder import is restricted to id1 pack files',
  filteredFolderFiles.join(',') === 'Quake/id1/pak0.pak', filteredFolderFiles.join(','));
if ((await localPaks()).length) {
  await page.evaluate(() => document.getElementById('pak-clear').click());
  await sleep(1500);
}
await page.evaluate(() => {
  const local = document.querySelector('input[name="game-data"][value="local"]');
  local.click();
});
check('local launch is blocked until data is imported',
  await page.$eval('#start', (button) => button.disabled));

// (1) upload before start -> boot registered
const status1 = await uploadPak(PAK_PATH);
check('upload before start stored', /pak1\.pak/.test(status1), status1);
check('UI says browser-local', /nothing uploaded|browser-local/.test(status1));
await startEngine();
check('boot picked up userdir pak (registered)',
  await waitForLog(/Playing registered version/), 'FS_Init search path');

// (2) live remove -> file gone + rescan ran. NOTE: the engine never resets
// the `registered` cvar back to 0 (vanilla FS_Rescan only sets it), so the
// rescan still prints "registered" — full downgrade needs a page reload
// (the UI says so). We assert the file removal + that a rescan happened.
logs.length = 0;
await page.evaluate(() => document.getElementById('pak-clear').click());
const rescanRan = await waitForLog(/Playing (registered|shareware) version/);
check('live remove: pak gone + fs_rescan ran',
  rescanRan && (await localPaks()).length === 0,
  'fs_rescan via _WebHost_RescanFS');

// (3) live re-upload -> rescan -> registered
logs.length = 0;
await uploadPak(PAK_PATH);
check('live upload rescans to registered',
  await waitForLog(/Playing registered version/));

// flush IDBFS before reload (upload already synced; belt & braces)
await page.evaluate(() => new Promise((r) => Module.FS.syncfs(false, r)));

// (4) reload -> persisted -> boot registered
await openPage(BASEURL + '?autostart=1');
check('pak persisted across reload',
  await waitForLog(/Playing registered version/), 'IDBFS round-trip');
check('persisted pak listed', (await localPaks()).includes('pak1.pak'));
check('no console errors (synthetic pak run)', errors.length === 0,
  errors.slice(0, 3).join(' | '));
await page.screenshot({ path: SCREENSHOT });

// (4b) explicit shareware choice must ignore, but not delete or move, the
// commercial/local-data mount retained by the browser.
await openPage(BASEURL + '?data=shareware&autostart=1');
check('explicit shareware launch uses isolated userdir',
  await waitForLog(/data profile: bundled shareware/));
check('explicit shareware launch ignores stored registered marker',
  await waitForLog(/Playing shareware version/));
check('shareware choice preserves browser-local pak',
  (await localPaks()).includes('pak1.pak'));

// (5) cleanup + reload -> shareware boot again
await page.evaluate(() => document.getElementById('pak-clear').click());
await sleep(1500);
check('cleanup removed pak', (await localPaks()).length === 0);
await openPage(BASEURL + '?autostart=1');
check('post-cleanup reload boots shareware',
  await waitForLog(/Playing shareware version/));

/* ---- optional: real KEX/GOG pak compat recon (report, not assert) ---- */
if (KEX_PAK && existsSync(KEX_PAK)) {
  console.log('# KEX pak compat recon: ' + KEX_PAK);
  // boot straight into e1m1 so the KEX progs.dat actually runs QC
  await openPage(BASEURL + '?args=' + encodeURIComponent('+skill 0 +map e1m1'));
  await page.waitForFunction(() => !document.getElementById('pak-input').disabled);
  const t0 = Date.now();
  const s = await uploadPak(KEX_PAK);
  console.log('  stored in ' + ((Date.now() - t0) / 1000).toFixed(1) + 's: ' + s);
  logs.length = 0; errors.length = 0;
  await startEngine();
  await sleep(20000);
  const probe = await page.evaluate(() => ({
    signon: Module._IN_Weapon_Probe(15),   // 4 = fully in game
    health: Module._IN_Weapon_Probe(14),
    shells: Module._IN_Weapon_Probe(13),
  }));
  console.log('  registered: ' + logs.some((l) => /Playing registered version/.test(l)));
  console.log('  in-game probe (signon/health/shells): '
    + probe.signon + '/' + probe.health + '/' + probe.shells);
  const prvm = logs.filter((l) => /PRVM|progs|error|Host_Error|QC/i.test(l)).slice(0, 20);
  console.log('  progs-related console lines:');
  console.log(prvm.map((e) => '    ' + e).join('\n') || '    (none)');
  console.log('  console errors: ' + errors.length);
  console.log(errors.slice(0, 10).map((e) => '    ' + e).join('\n'));
  await page.screenshot({ path: 'm4-kex-recon.png' });
  // cleanup so the shareware state is restored (recon only)
  await page.evaluate(() => document.getElementById('pak-clear').click());
  await sleep(3000);
  console.log('  (KEX pak removed again — recon only)');
}

await browser.close();
console.log(`\n# verdict: ${failCount === 0 ? 'OK' : 'FAIL'} (${passCount} pass, ${failCount} fail)`);
process.exit(failCount === 0 ? 0 : 1);
