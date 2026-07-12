#!/usr/bin/env node
/*
 * m4-pwa-test.mjs — PWA installability + offline support (M4 task #7).
 *
 * Verifies the service-worker/manifest shell added in src/web-page/:
 *   (A) online: manifest.webmanifest fetches 200 and parses with the right
 *       fields; the service worker registers and reaches 'activated'; every
 *       PRECACHE_URLS entry from sw.js ends up in the versioned cache; no
 *       console errors.
 *   (B) offline: page.setOfflineMode(true) + reload -> the page still loads
 *       (served entirely from the SW cache), self.crossOriginIsolated is
 *       still true, and the engine still boots (waits for the
 *       '[web-host] engine initialised' console line, ?autostart=1).
 *   (C) update path: the BUILD_VERSION stamped into the on-disk web/sw.js is
 *       bumped (simulating a new build), an explicit
 *       registration.update() is issued while online, and the old
 *       version's cache must be deleted while a new one (matching the new
 *       stamped version) is populated with the same precache set. The file
 *       on disk is restored afterwards regardless of outcome.
 *
 * This test runs its own local static server (reusing web/serve.mjs, same
 * COOP/COEP headers as production) on a dedicated port so it can't collide
 * with any server another suite left running on 8090-8095, and so its
 * service-worker registration/cache storage is isolated by origin.
 *
 * Prereqs: a build in web/ (src/build.sh). Env: QQ_PUPPETEER, QQ_CHROME,
 * QQ_PWA_PORT (default 8096).
 * Usage: node m4-pwa-test.mjs [screenshot.png]
 */
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { spawn } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = join(HERE, '..', '..', '..');          // webxr-port/
const WEB_DIR = join(ROOT, 'web');
const SERVE_SCRIPT = join(WEB_DIR, 'serve.mjs');
const SW_PATH = join(WEB_DIR, 'sw.js');

const PORT = process.env.QQ_PWA_PORT || '8096';
const BASEURL = `http://localhost:${PORT}/`;
const URL = BASEURL + '?autostart=1';
const SCREENSHOT = process.argv[2] || 'm4-pwa-test.png';

const PUPPETEER_DIR = process.env.QQ_PUPPETEER
  || 'C:/Users/Caldor/.claude/skills/webxr-test/node_modules/puppeteer-core';
const { default: puppeteer } = await import(
  'file:///' + PUPPETEER_DIR.replace(/\\/g, '/') + '/lib/esm/puppeteer/puppeteer-core.js');

const CHROME = [process.env.QQ_CHROME,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe']
  .filter(Boolean).find(existsSync);
if (!CHROME) { console.error('no chrome found (set QQ_CHROME)'); process.exit(2); }
if (!existsSync(SW_PATH)) { console.error('web/sw.js not found — run src/build.sh first'); process.exit(2); }

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let passCount = 0, failCount = 0;
function check(name, ok, detail = '') {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? ' — ' + detail : ''}`);
  ok ? passCount++ : failCount++;
}
async function pollUntil(fn, timeoutMs = 15000, intervalMs = 400) {
  const t0 = Date.now();
  let last;
  while (Date.now() - t0 < timeoutMs) {
    last = await fn();
    if (last) return last;
    await sleep(intervalMs);
  }
  return last;
}

/* ---- own local server (isolated port, same headers as production) ---- */
console.log(`# starting local server on port ${PORT}`);
const server = spawn(process.execPath, [SERVE_SCRIPT, PORT], { cwd: WEB_DIR });
let serverReady = false;
server.stdout.on('data', (d) => { if (/serving/.test(d.toString())) serverReady = true; });
server.stderr.on('data', (d) => process.stderr.write('[serve] ' + d));
server.on('exit', (code) => { if (code && code !== null) console.error(`[serve] exited early: ${code}`); });
await pollUntil(() => serverReady, 10000, 200);
if (!serverReady) { console.error('local server did not start in time'); process.exit(2); }

const browser = await puppeteer.launch({
  executablePath: CHROME, headless: true, args: ['--no-sandbox'],
});

/* forward console output from any service-worker target too (best-effort —
 * sw.js has no console.log calls today, but a failed install would show up
 * here as an uncaught rejection) */
const swErrors = [];
browser.on('targetcreated', async (target) => {
  if (target.type() !== 'service_worker') return;
  try {
    const worker = await target.worker();
    worker?.on('console', (m) => { if (m.type() === 'error') swErrors.push(m.text()); });
  } catch { /* not fatal — best-effort */ }
});

const errors = [];
const consoleLines = [];
const page = await browser.newPage();
page.setDefaultTimeout(45000);
page.on('console', (m) => {
  consoleLines.push(m.text());
  if (m.type() === 'error') errors.push(m.text());
});
page.on('pageerror', (e) => errors.push('PAGEERROR: ' + e.message));

async function waitForLog(rx, timeoutMs = 30000) {
  const t0 = Date.now();
  while (Date.now() - t0 < timeoutMs) {
    if (consoleLines.some((l) => rx.test(l))) return true;
    await sleep(250);
  }
  return false;
}

let exitCode = 1;
try {
  /* =================== (A) online install =================== */
  console.log('# (A) online: loading ' + URL);
  await page.goto(URL, { waitUntil: 'load' });

  const manifestRes = await page.evaluate(async () => {
    const r = await fetch('./manifest.webmanifest');
    return { status: r.status, json: r.ok ? await r.json() : null };
  });
  check('manifest.webmanifest fetches 200', manifestRes.status === 200, 'status=' + manifestRes.status);
  check('manifest parses with expected fields',
    !!manifestRes.json
      && manifestRes.json.name === 'QuakeQuest Web'
      && manifestRes.json.short_name === 'QuakeQuest'
      && manifestRes.json.display === 'standalone'
      && Array.isArray(manifestRes.json.icons) && manifestRes.json.icons.length === 3,
    JSON.stringify(manifestRes.json));
  check('manifest has a maskable icon',
    !!manifestRes.json?.icons?.some((i) => i.purpose === 'maskable'));

  const swSupported = await page.evaluate(() => 'serviceWorker' in navigator);
  check('serviceWorker supported in this browser', swSupported);

  // registration happens from the page's own 'load' listener (index.html),
  // which races Puppeteer's goto({waitUntil:'load'}) resolution — poll.
  const regAny = await pollUntil(() => page.evaluate(async () => {
    const r = await navigator.serviceWorker.getRegistration();
    return !!r;
  }), 8000, 250);
  check('service worker registered', regAny);
  // navigator.serviceWorker.ready resolves as soon as .active exists, which
  // can still be in the 'activating' state — poll until it's fully
  // 'activated' (that's also what gates cache population below).
  const reg = await pollUntil(() => page.evaluate(async () => {
    const ready = await navigator.serviceWorker.ready;
    const state = ready.active ? ready.active.state : null;
    return state === 'activated' ? { scope: ready.scope, state } : null;
  }), 20000, 300);
  check('service worker reached "activated"', reg?.state === 'activated', JSON.stringify(reg));

  const cacheInfo = await pollUntil(() => page.evaluate(async () => {
    const keys = await caches.keys();
    const name = keys.find((k) => k.startsWith('quakequest-'));
    if (!name) return null;
    const cache = await caches.open(name);
    const reqs = await cache.keys();
    return { cacheName: name, paths: reqs.map((r) => new URL(r.url).pathname).sort() };
  }), 15000, 300);
  check('a quakequest-<version> cache exists', !!cacheInfo, JSON.stringify(cacheInfo));
  const expectCount = 8; // ./, quake.js, quake.wasm, quake.data, manifest, 3 icons
  check('precache holds all 8 expected entries', cacheInfo?.paths?.length === expectCount,
    JSON.stringify(cacheInfo?.paths));
  const oldCacheName = cacheInfo?.cacheName;

  check('engine booted on first load', await waitForLog(/\[web-host\] engine initialised/));
  check('no console errors (online load)', errors.length === 0, errors.slice(0, 5).join(' | '));

  /* =================== (B) offline reload =================== */
  console.log('# (B) offline reload');
  errors.length = 0; consoleLines.length = 0;
  await page.setOfflineMode(true);
  await page.reload({ waitUntil: 'load' });

  const isolated = await page.evaluate(() => self.crossOriginIsolated);
  check('crossOriginIsolated === true after offline reload', isolated === true, String(isolated));
  check('engine booted from SW cache while offline',
    await waitForLog(/\[web-host\] engine initialised/));
  check('no console errors (offline reload)', errors.length === 0, errors.slice(0, 5).join(' | '));
  await page.screenshot({ path: SCREENSHOT });

  await page.setOfflineMode(false);

  /* =================== (C) update path =================== */
  console.log('# (C) update path (bump stamped BUILD_VERSION on disk)');
  const original = readFileSync(SW_PATH, 'utf8');
  let restored = false;
  try {
    const newVersion = 'testbump' + Date.now().toString(36);
    const bumped = original.replace(
      /const BUILD_VERSION = '[^']+';/,
      `const BUILD_VERSION = '${newVersion}';`,
    );
    check('found BUILD_VERSION line to bump in web/sw.js', bumped !== original);
    writeFileSync(SW_PATH, bumped);

    errors.length = 0;
    await page.evaluate(async () => {
      const reg = await navigator.serviceWorker.getRegistration();
      await reg.update();
    });

    // Poll for the FULL swap (old gone + new cache populated) in one shot —
    // caches.open() registers a cache name before it's populated, and old
    // caches are only deleted in 'activate' (after install's addAll already
    // resolved), so checking either half alone races the install/activate
    // sequence and can observe a half-finished swap.
    const newCacheName = 'quakequest-' + newVersion;
    const swapped = await pollUntil(() => page.evaluate(async (args) => {
      const [oldName, newName] = args;
      const keys = await caches.keys();
      const oldGone = !keys.includes(oldName);
      let newPaths = [];
      if (keys.includes(newName)) {
        const cache = await caches.open(newName);
        const reqs = await cache.keys();
        newPaths = reqs.map((r) => new URL(r.url).pathname).sort();
      }
      const done = oldGone && newPaths.length > 0;
      return done ? { keys, oldGone, newPaths } : null;
    }, [oldCacheName, newCacheName]), 20000, 500);
    check('update installed a NEW cache for the bumped version',
      !!swapped?.keys?.includes(newCacheName), JSON.stringify(swapped));
    check('update deleted the OLD version cache',
      !!swapped?.oldGone, JSON.stringify(swapped));
    check('new cache holds all 8 precache entries', swapped?.newPaths?.length === expectCount,
      JSON.stringify(swapped?.newPaths));

    check('no console errors during update', errors.length === 0, errors.slice(0, 5).join(' | '));
    check('no service-worker-side errors during update', swErrors.length === 0,
      swErrors.slice(0, 5).join(' | '));
  } finally {
    writeFileSync(SW_PATH, original);
    restored = readFileSync(SW_PATH, 'utf8') === original;
  }
  check('web/sw.js restored to its original (build.sh-stamped) content', restored);

  exitCode = failCount === 0 ? 0 : 1;
} catch (e) {
  console.error('# harness error: ' + (e?.stack || e));
  failCount++;
  exitCode = 1;
} finally {
  await browser.close();
  server.kill();
}

console.log(`\n# verdict: ${exitCode === 0 ? 'OK' : 'FAIL'} (${passCount} pass, ${failCount} fail)`);
process.exit(exitCode);
