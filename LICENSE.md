# License

This directory contains:

- `darkplaces/` — the DarkPlaces engine as forked by QuakeQuest (Team Beef /
  DrBeef, upstream by LadyHavoc / id Software), **GPL-2.0-or-later**. The full
  license text is in `darkplaces/COPYING`. Modifications made for the WebXR
  port are marked with `WEBXR-PORT` comments and documented in PORT_NOTES.md.
- `web-host/` — new code written for this port (browser host replacing the
  Android/OpenXR host layer), licensed **GPL-2.0-or-later** to match the
  engine it links with.

Source: https://github.com/DrBeef/QuakeQuest (engine fork under
Projects/Android/jni/darkplaces).

The shareware Quake game data (`data/id1/pak0.pak`, distributed alongside but
not in this directory) is not GPL; it is the id Software shareware release
v1.06, redistributable as shareware per its original terms.
