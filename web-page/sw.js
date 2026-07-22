// sw.js — QuakeQuest Web service worker: versioned precache for offline/PWA use.
//
// LEGAL / PRIVACY CONSTRAINT — read before touching PRECACHE_URLS:
// This list must ONLY ever contain the engine (quake.js/quake.wasm) and the
// freely redistributable SHAREWARE game data (quake.data), plus the page
// shell/manifest/icons. User-supplied full-game pak files (GOG/Steam/etc,
// see index.html's "Full game data" picker) are written straight into an
// IndexedDB-backed IDBFS mount by the page's own JS — they never travel
// through an HTTP request this service worker could observe, and they must
// NEVER be added to PRECACHE_URLS or handled by any fetch route below.
//
// BUILD_VERSION is a placeholder substituted by build.sh (sed) with a short
// hash of quake.wasm + quake.data + index.html, so the cache name changes
// automatically whenever any of those change. Never bump this by hand.
const BUILD_VERSION = '__BUILD_VERSION__';
// Cache Storage is origin-wide, not service-worker-scope-wide. Include the
// registration scope so the legacy /webxr/quakequest/ compatibility alias
// and /webxr/Ports/QuakeQuest/ can coexist without deleting or reading each
// other's app shell during migration.
const SCOPE_KEY = encodeURIComponent(new URL(self.registration.scope).pathname);
const CACHE_PREFIX = 'quakequest-' + SCOPE_KEY + '-';
const CACHE_NAME = CACHE_PREFIX + BUILD_VERSION;
const LEGACY_CACHE_PATTERN = /^quakequest-[0-9a-f]{12}$/i;

const PRECACHE_URLS = [
  './',                          // index.html (the whole app shell)
  './quake.js',
  './quake.wasm',
  './quake.data',                // shareware pak0.pak, bundled — SEE CONSTRAINT ABOVE
  './manifest.webmanifest',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/icon-512-maskable.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE_NAME);
    // cache:'reload' bypasses the HTTP cache so a fresh deploy is actually
    // fresh in here too, not a stale disk-cached copy of the old build.
    await cache.addAll(PRECACHE_URLS.map((u) => new Request(u, { cache: 'reload' })));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const names = await caches.keys();
    await Promise.all(
      names.filter((n) => (n.startsWith(CACHE_PREFIX) && n !== CACHE_NAME)
             || LEGACY_CACHE_PATTERN.test(n))
           .map((n) => caches.delete(n)),
    );
    await self.clients.claim();
  })());
});

// The app requires cross-origin isolation (SharedArrayBuffer/pthreads).
// Responses read back from the Cache API keep whatever headers they had at
// cache-put time (the server already sends COOP/COEP — see deploy/.htaccess
// and web/serve.mjs), but for navigations we defensively re-wrap the
// response and force both headers so an offline reload still reports
// crossOriginIsolated === true even if something upstream ever changes.
function withIsolationHeaders(response) {
  const headers = new Headers(response.headers);
  headers.set('Cross-Origin-Opener-Policy', 'same-origin');
  headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
  return new Response(response.body, {
    status: response.status,
    statusText: response.statusText,
    headers,
  });
}

self.addEventListener('fetch', (event) => {
  const req = event.request;
  if (req.method !== 'GET') return;
  const url = new URL(req.url);
  if (url.origin !== self.location.origin) return;

  // Navigations: this is a single-page app, so any in-scope navigation
  // (regardless of path/query — e.g. ?autostart=1, ?args=...) is served the
  // cached app shell. Falls back to network (then lets the browser show its
  // normal offline error) if the shell was never precached.
  if (req.mode === 'navigate') {
    event.respondWith((async () => {
      const cache = await caches.open(CACHE_NAME);
      const shell = await cache.match('./');
      if (shell) return withIsolationHeaders(shell);
      const netResp = await fetch(req);
      return withIsolationHeaders(netResp);
    })());
    return;
  }

  // Everything else same-origin: cache-first against the precached set,
  // network fallback (covers anything not in PRECACHE_URLS, e.g. future
  // assets) — never cache-writes at fetch time, so this can't accidentally
  // grow to include user pak data.
  event.respondWith((async () => {
    const cache = await caches.open(CACHE_NAME);
    const cached = await cache.match(req);
    if (cached) return cached;
    return fetch(req);
  })());
});
