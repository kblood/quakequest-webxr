# WebXR ports deployment

## Completed migration

The Ports layout was deployed on 2026-07-22. The canonical release is now
`https://dionysus.dk/webxr/Ports/QuakeQuest/`, with the site catalog at
`https://dionysus.dk/webxr/Ports/`. Apache internally aliases the legacy
`/webxr/quakequest/` scope to the same physical release so existing PWAs and
the signed Quest TWA continue to work.

The canonical files live at `/var/www/html/webxr/Ports/QuakeQuest`. The exact
pre-migration release is preserved at
`/var/www/html/webxr/.quakequest.pre-ports-20260722-182834`; the previous Apache
configuration is preserved as
`/etc/apache2/conf-available/quakequest.conf.pre-ports-20260722-182834`.
Both live paths returned byte-identical HTML, service worker, WASM, and
shareware data after deployment. Both reported independent scope-qualified
service-worker caches, `crossOriginIsolated === true`, two game-data choices,
two display choices, and no browser page errors.

The data-free Surreal Engine release is at
`https://dionysus.dk/webxr/Ports/SurrealEngine/`. It was updated on 2026-07-24
from clean `kblood/SurrealEngine` integration commit `e9031169`. The live-origin
browser smoke verified cross-origin isolation, WASM MIME, the local-folder
import gate, ordinary keyboard/mouse and fullscreen behavior, synthetic flat
launch, and the matching corresponding-source archive. Local UT99 348, Unreal
Special Edition 200, and Deus Ex 1002f demo folders are recognized, but no UE1
game or demo data is hosted. Physical Quest/WebXR behavior remains a separate
hardware gate.

Surreal Engine uses one public release directory. Old numbered candidate and
experimental directories were moved intact to the non-public server archive
`/var/www/html/webxr/.SurrealEngine-public-archive-pre-single-20260724`; the
previous stable release is at
`/var/www/html/webxr/.SurrealEngine.rollback-04687fe1-20260724`.

## Verified pre-migration release

Before the migration, QuakeQuest was released at:

```text
https://dionysus.dk/webxr/quakequest/
```

The live URL returns HTTP 200 with `Cross-Origin-Opener-Policy: same-origin`
and `Cross-Origin-Embedder-Policy: require-corp`. On 2026-07-22 the live
`index.html`, `sw.js`, `quake.wasm`, and `quake.data` SHA-256 hashes exactly
matched `C:\Devstuff\QuestGames\webxr-port\deploy\stage`. The server's
`/webxr/` index also links `quakequest/`. The verified physical release path is
`/var/www/html/webxr/quakequest`.

The public source repository is
`https://github.com/kblood/quakequest-webxr.git`. At inspection time it
contained source and page assets but no site/vhost repository, deployment
workflow, or server credentials; the staging directory and production Apache
`.htaccess` were outside Git. This change tracks the app's reviewed per-folder
`.htaccess`, but the virtual-host alias and deployment authority remain absent.
Therefore this repository documents the remote migration but does not perform
or pretend to perform it.

## Canonical ports layout

Use this physical and URL layout:

```text
/var/www/html/webxr/Ports/
├── QuakeQuest/       -> https://dionysus.dk/webxr/Ports/QuakeQuest/
└── SurrealEngine/    -> https://dionysus.dk/webxr/Ports/SurrealEngine/
```

Do not add numbered Surreal release directories beneath `Ports`. Update the
single `SurrealEngine` directory by staging, hash verification, and atomic
rename; keep rollback copies outside the public catalog.

QuakeQuest uses only relative application URLs: `quake.js`, `quake.wasm`,
`quake.data`, the manifest, icons, service-worker registration, and precache
requests all resolve within the directory from which the page was served. The
manifest uses relative `start_url` and `scope`. No generated file needs the
deployment prefix compiled into it.

## Preserve the old URL and installed Quest TWA

Do not replace `/webxr/quakequest/` with only an HTTP redirect. Existing Quest
PWA installations can still be controlled by the old service-worker scope, and
the already signed Quest TWA uses this old start URL and scope. A redirect alone
can strand those cached installations.

Physically store one release at `webxr/Ports/QuakeQuest` and configure the old
URL as an internal Apache alias to that same directory. For example, in the
existing dionysus.dk virtual host (the actual vhost filename must be verified on
the server rather than guessed):

```apache
Alias "/webxr/quakequest/" "/var/www/html/webxr/Ports/QuakeQuest/"

<Directory "/var/www/html/webxr/Ports/QuakeQuest">
    Require all granted
    AllowOverride FileInfo
</Directory>
```

The canonical URL works normally through the document root. The alias keeps
the browser-visible old URL, manifest scope, service-worker URL, IndexedDB
origin, and TWA start URL valid while serving the same bytes. Keep the current
root `.well-known/assetlinks.json`; do not rebuild or re-sign the TWA merely for
this site-layout migration.

Cache Storage is shared across an origin even when service-worker scopes
differ. `web-page/sw.js` therefore includes its encoded registration scope in
the cache name. Old and canonical scopes can install simultaneously without
deleting or reading each other's app shell. On activation it also removes the
old unscoped twelve-hex-character cache left by pre-migration builds.

## Reviewed remote migration sequence

No step below should be run until the operator has confirmed the vhost and
backup paths on the server.

1. Build from this repository into a new local staging directory. Confirm that
   `quake.data` contains only the redistributable shareware bundle; never copy a
   user's full-game files into Git, staging, or the server. The build copies the
   reviewed `web-page/.htaccess` into the output; do not substitute an unrelated
   site-wide header file.
2. Upload into a new sibling directory under `/var/www/html/webxr/Ports`, verify
   file counts and hashes, then atomically rename it to `QuakeQuest`. Preserve a
   timestamped backup of the current `/var/www/html/webxr/quakequest` release.
3. Add the internal alias above to the verified dionysus.dk vhost, run Apache's
   configuration test, and reload Apache only if that test succeeds.
4. Verify both URLs return 200 and byte-identical `index.html`, `sw.js`, WASM,
   and data; both must send COOP/COEP and correct WASM/manifest MIME types.
5. At each URL, verify `navigator.serviceWorker.ready.scope` matches that URL,
   `crossOriginIsolated === true`, the cache name contains the encoded scope,
   flatscreen launch works, and WebXR entry works on a headset.
6. Verify an existing installed Quest PWA/TWA that still opens
   `/webxr/quakequest/`. Keep the alias indefinitely unless a separately signed
   TWA release changes its scope and the PWA migration has been measured.

Rollback is to disable the alias, restore the backed-up physical old directory,
test Apache configuration, and reload. Do not delete the backup as part of the
initial migration.

## Launcher data boundaries

The deployed build contains only shareware. A local-game launch imports only
`.pak`/`.pk3` files selected from the user's `id1` directory into browser-local
IndexedDB at `/quake_user/id1`; no upload endpoint exists. When those local
packs exist, shareware launches use the separate `/quake_shareware_user` IDBFS
mount, so selecting the demo does not silently activate commercial data. With
no local packs the original `/quake_user` profile remains in use, preserving
existing shareware saves/config. The repository, deployment stage, and
migration never copy or move commercial game data.
