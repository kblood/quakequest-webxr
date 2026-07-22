#!/usr/bin/env node
/* Prove that the legacy and canonical deployment paths can serve one build
 * while retaining independent service-worker scopes and caches. No game data
 * is created or copied by this test; it serves the existing local web build. */
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import { dirname, extname, join, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const WEB_DIR = join(HERE, '..', '..', '..', 'web');
const PORT = Number(process.env.QQ_SCOPE_PORT || 8097);
const ORIGIN = `http://localhost:${PORT}`;
const PREFIXES = ['/webxr/quakequest/', '/webxr/Ports/QuakeQuest/'];
const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm', '.data': 'application/octet-stream',
  '.webmanifest': 'application/manifest+json', '.png': 'image/png',
};

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

const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, ORIGIN).pathname);
    const prefix = PREFIXES.find((candidate) => pathname.startsWith(candidate));
    if (!prefix) { res.writeHead(404); res.end('not found'); return; }
    let relative = pathname.slice(prefix.length) || 'index.html';
    const file = normalize(join(WEB_DIR, relative));
    if (!file.startsWith(WEB_DIR) || !(await stat(file)).isFile()) {
      res.writeHead(404); res.end('not found'); return;
    }
    const body = await readFile(file);
    res.writeHead(200, {
      'Content-Type': MIME[extname(file)] || 'application/octet-stream',
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cache-Control': 'no-store',
    });
    res.end(body);
  } catch {
    res.writeHead(404); res.end('not found');
  }
});
await new Promise((resolve) => server.listen(PORT, resolve));

let pass = 0;
let fail = 0;
function check(name, ok, detail = '') {
  console.log(`  [${ok ? 'PASS' : 'FAIL'}] ${name}${detail ? ' — ' + detail : ''}`);
  ok ? pass++ : fail++;
}
async function scopeState(page) {
  await page.waitForFunction(async () => {
    const registration = await navigator.serviceWorker.getRegistration();
    return registration?.active?.state === 'activated';
  }, { timeout: 30000 });
  return page.evaluate(async () => {
    const registration = await navigator.serviceWorker.getRegistration();
    const keys = await caches.keys();
    const scopeKey = encodeURIComponent(new URL(registration.scope).pathname);
    const prefix = 'quakequest-' + scopeKey + '-';
    const cacheName = keys.find((key) => key.startsWith(prefix));
    const requests = cacheName ? await (await caches.open(cacheName)).keys() : [];
    return {
      scope: registration.scope,
      cacheName,
      paths: requests.map((request) => new URL(request.url).pathname).sort(),
      allKeys: keys.sort(),
    };
  });
}

let browser;
try {
  browser = await puppeteer.launch({ executablePath: CHROME, headless: true, args: ['--no-sandbox'] });
  const states = [];
  for (const prefix of PREFIXES) {
    const page = await browser.newPage();
    await page.goto(ORIGIN + prefix, { waitUntil: 'load' });
    states.push(await scopeState(page));
  }
  check('legacy scope remains registered', states[0].scope === ORIGIN + PREFIXES[0], states[0].scope);
  check('canonical Ports scope is registered', states[1].scope === ORIGIN + PREFIXES[1], states[1].scope);
  check('scope-qualified caches are distinct',
    !!states[0].cacheName && !!states[1].cacheName && states[0].cacheName !== states[1].cacheName,
    states.map((state) => state.cacheName).join(' | '));
  check('legacy scope precaches only legacy paths',
    states[0].paths.length === 8 && states[0].paths.every((path) => path.startsWith(PREFIXES[0])),
    JSON.stringify(states[0].paths));
  check('canonical scope precaches only canonical paths',
    states[1].paths.length === 8 && states[1].paths.every((path) => path.startsWith(PREFIXES[1])),
    JSON.stringify(states[1].paths));
  check('activating canonical scope did not delete legacy cache',
    states[1].allKeys.includes(states[0].cacheName), JSON.stringify(states[1].allKeys));
} finally {
  if (browser) await browser.close();
  await new Promise((resolve) => server.close(resolve));
}

console.log(`\n# verdict: ${fail ? 'FAIL' : 'OK'} (${pass} pass, ${fail} fail)`);
process.exit(fail ? 1 : 0);
