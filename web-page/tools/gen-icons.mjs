#!/usr/bin/env node
/*
 * gen-icons.mjs — rasterizes the hand-authored SVG sources in this directory
 * to the PNG sizes the PWA manifest needs. Run this manually whenever
 * icon.svg / icon-maskable.svg change; the PNGs it writes to ../icons/ are
 * committed to git and copied verbatim by build.sh (no rasterization happens
 * at build time).
 *
 * Reuses the webxr-test skill's Chrome detection (no separate Chromium
 * download — puppeteer-core drives the system browser).
 *
 * Usage: node gen-icons.mjs
 */
import { readFileSync, mkdirSync, existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ICONS_DIR = join(HERE, '..', 'icons');
mkdirSync(ICONS_DIR, { recursive: true });

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

const TARGETS = [
  { file: 'icon.svg', out: 'icon-192.png', size: 192 },
  { file: 'icon.svg', out: 'icon-512.png', size: 512 },
  { file: 'icon-maskable.svg', out: 'icon-512-maskable.png', size: 512 },
];

const browser = await puppeteer.launch({ executablePath: CHROME, headless: true, args: ['--no-sandbox'] });
const page = await browser.newPage();

for (const t of TARGETS) {
  const svg = readFileSync(join(HERE, t.file), 'utf8');
  await page.setViewport({ width: t.size, height: t.size, deviceScaleFactor: 1 });
  await page.setContent(
    `<!doctype html><html><head><style>
       html,body{margin:0;padding:0;width:${t.size}px;height:${t.size}px;background:#111;}
       svg{display:block;width:${t.size}px;height:${t.size}px;}
     </style></head><body>${svg}</body></html>`,
    { waitUntil: 'load' },
  );
  const out = join(ICONS_DIR, t.out);
  await page.screenshot({ path: out, clip: { x: 0, y: 0, width: t.size, height: t.size }, omitBackground: false });
  console.log(`  wrote ${out} (${t.size}x${t.size} from ${t.file})`);
}

await browser.close();
console.log('# icons generated');
