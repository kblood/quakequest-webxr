#!/bin/bash
# build.sh — Emscripten build for the QuakeQuest->Web port.
# vbo-modernization branch: builds with real VBOs (no -sFULL_ES2), output
# kept local to this worktree (webdist/) instead of the shared ../web.
#
# Usage:   ./build.sh [--audio=null|sdl] [--clean] [-j N]
# Output:  webdist/quake.js, quake.wasm, quake.data
#
# Compilation-unit list derived from QuakeQuest Projects/Android/jni/Android.mk,
# minus the Android host (QuakeQuestSrc/*), sys_linux.c (replaced by
# web-host/sys_web.c) and snd_opensl.c (replaced by snd_null.c or snd_sdl.c).
set -e
cd "$(dirname "$0")"

AUDIO=sdl
JOBS=8
CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --audio=null) AUDIO=null ;;
    --audio=sdl)  AUDIO=sdl ;;
    --clean)      CLEAN=1 ;;
    -j*)          JOBS="${arg#-j}" ;;
  esac
done

# Activate emsdk if emcc isn't on PATH (per-shell activation on this box)
if ! command -v emcc >/dev/null 2>&1; then
  EMSDK_QUIET=1 source /c/Devstuff/emsdk/emsdk_env.sh
fi

OBJ=build/obj
WEBOUT=../web
[ "$CLEAN" = 1 ] && rm -rf build
mkdir -p "$OBJ" "$WEBOUT"

# --- engine sources (from Android.mk SRC_COMMON + SRC_QUEST + top-level) ---
SRCS="
darkplaces/builddate.c
darkplaces/vid_android.c
darkplaces/thread_pthread.c
darkplaces/cd_null.c
darkplaces/cd_shared.c
darkplaces/bih.c
darkplaces/cap_avi.c
darkplaces/cap_ogg.c
darkplaces/crypto.c
darkplaces/cl_collision.c
darkplaces/cl_demo.c
darkplaces/cl_dyntexture.c
darkplaces/cl_input.c
darkplaces/cl_main.c
darkplaces/cl_parse.c
darkplaces/cl_particles.c
darkplaces/cl_screen.c
darkplaces/cl_video.c
darkplaces/clvm_cmds.c
darkplaces/cmd.c
darkplaces/collision.c
darkplaces/common.c
darkplaces/console.c
darkplaces/csprogs.c
darkplaces/curves.c
darkplaces/cvar.c
darkplaces/dpsoftrast.c
darkplaces/dpvsimpledecode.c
darkplaces/filematch.c
darkplaces/fractalnoise.c
darkplaces/fs.c
darkplaces/ft2.c
darkplaces/utf8lib.c
darkplaces/gl_backend.c
darkplaces/gl_draw.c
darkplaces/gl_rmain.c
darkplaces/gl_rsurf.c
darkplaces/gl_textures.c
darkplaces/hmac.c
darkplaces/host.c
darkplaces/host_cmd.c
darkplaces/image.c
darkplaces/image_png.c
darkplaces/jpeg.c
darkplaces/keys.c
darkplaces/lhnet.c
darkplaces/libcurl.c
darkplaces/mathlib.c
darkplaces/matrixlib.c
darkplaces/mdfour.c
darkplaces/menu.c
darkplaces/meshqueue.c
darkplaces/mod_skeletal_animatevertices_sse.c
darkplaces/mod_skeletal_animatevertices_generic.c
darkplaces/model_alias.c
darkplaces/model_brush.c
darkplaces/model_shared.c
darkplaces/model_sprite.c
darkplaces/mvm_cmds.c
darkplaces/netconn.c
darkplaces/palette.c
darkplaces/polygon.c
darkplaces/portals.c
darkplaces/protocol.c
darkplaces/prvm_cmds.c
darkplaces/prvm_edict.c
darkplaces/prvm_exec.c
darkplaces/r_explosion.c
darkplaces/r_lerpanim.c
darkplaces/r_lightning.c
darkplaces/r_lasersight.c
darkplaces/r_modules.c
darkplaces/r_shadow.c
darkplaces/r_sky.c
darkplaces/r_sprites.c
darkplaces/sbar.c
darkplaces/snprintf.c
darkplaces/sv_demo.c
darkplaces/sv_main.c
darkplaces/sv_move.c
darkplaces/sv_phys.c
darkplaces/sv_user.c
darkplaces/svbsp.c
darkplaces/svvm_cmds.c
darkplaces/sys_shared.c
darkplaces/vid_shared.c
darkplaces/view.c
darkplaces/wad.c
darkplaces/world.c
darkplaces/zone.c
web-host/sys_web.c
web-host/main_web.c
web-host/fbo_smoketest.c
web-host/webxr_bridge.c
web-host/webxr_input.c
"

# --- audio backend ---
if [ "$AUDIO" = "sdl" ]; then
  SRCS="$SRCS
darkplaces/snd_sdl.c
darkplaces/snd_main.c
darkplaces/snd_mem.c
darkplaces/snd_mix.c
darkplaces/snd_ogg.c
darkplaces/snd_modplug.c
darkplaces/snd_wav.c
"
  AUDIO_CFLAGS="-sUSE_SDL=2"
  AUDIO_LDFLAGS="-sUSE_SDL=2"
else
  SRCS="$SRCS
darkplaces/snd_null.c
"
  AUDIO_CFLAGS=""
  AUDIO_LDFLAGS=""
fi

CFLAGS="-O2 -std=gnu17 -include stdbool.h -Idarkplaces -w \
  -sUSE_OGG=1 -sUSE_VORBIS=1 $AUDIO_CFLAGS"
if [ "$VBO_DEBUG" = 1 ]; then
  CFLAGS="$CFLAGS -DWEBXR_PORT_VBO_DEBUG"
fi

# vbo-modernization: -sFULL_ES2 dropped. The engine's forcevbo mechanism
# (gl_webgl_forcevbo, see src/darkplaces/gl_backend.c) now routes every draw
# through a real VBO, so FULL_ES2's client-array emulation shim (a
# scratch-buffer copy per draw call) is no longer needed. See
# reports/07-vbo-modernization.md.
LDFLAGS="-sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
  -sINITIAL_MEMORY=256MB -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8MB \
  -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,UTF8ToString \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
  --js-library web-host/lib/webxr/library_webxr.js \
  -lidbfs.js \
  -sASSERTIONS=1 \
  -sGROWABLE_ARRAYBUFFERS=0 \
  --profiling-funcs \
  -sUSE_OGG=1 -sUSE_VORBIS=1 $AUDIO_LDFLAGS \
  --preload-file ../data/id1@/quake/id1"

# --- compile (parallel, skip up-to-date objects) ---
echo "== compiling (audio=$AUDIO, jobs=$JOBS) =="
NEED=""
OBJS=""
for src in $SRCS; do
  obj="$OBJ/$(basename "${src%.c}").o"
  OBJS="$OBJS $obj"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    NEED="$NEED $src"
  fi
done

if [ -n "$NEED" ]; then
  printf '%s\n' $NEED | xargs -P "$JOBS" -I{} sh -c '
    src="{}"
    obj="'"$OBJ"'/$(basename "${src%.c}").o"
    echo "  CC $src"
    emcc '"$CFLAGS"' -c "$src" -o "$obj" || { echo "FAILED: $src"; exit 255; }
  '
fi

# --- link ---
echo "== linking =="
emcc $OBJS -o "$WEBOUT/quake.js" $LDFLAGS
echo "== done: $WEBOUT/quake.js + quake.wasm + quake.data =="
