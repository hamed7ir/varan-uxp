#!/bin/sh
# -----------------------------------------------------------------------------
# Varan v1.1: Windows-ARM32 ffvpx assembler wrapper.
#
# WHY THIS EXISTS. autoconf.mk sets `AS = :` on this target -- a shell no-op --
# so the build system has NO working assembler and every .S is silently skipped.
# That is why media/ffvpx/libavcodec/arm/*.S and libavutil/arm/*.S were present
# in SOURCES yet never produced an object.
#
# Scoped AS override for the ffvpx arm directories ONLY, set in each dir's
# Makefile.in. Same seam libffi already uses (config/external/ffi/armasm-wrap.sh,
# wired at config/external/ffi/Makefile.in:22).
#
# SIMPLER THAN THE LIBFFI WRAPPER, deliberately. libffi's file is armasm syntax
# and needs a preprocess-then-armasm dance. ffmpeg's .S are GNU-as syntax, and
# clang-cl assembles those directly for thumbv7-windows-msvc -- MEASURED
# 2026-09-15: all 15 ffvpx ARM .S assemble to ARMNT 0x1C4 with ZERO A32
# relocations, including the ARMv6 SIMD ones. So this only has to translate the
# GNU-style recipe into clang-cl's spelling.
#
# Invoked by config/rules.mk (SSRCS rule):
#     $(AS) -o $@ $(DEFINES) $(ASFLAGS) $(LOCAL_INCLUDES) -c $<
# clang-cl wants -Fo<obj> rather than -o <obj>, and needs -x assembler-with-cpp
# because these .S carry #include and #if directives.
#
# DIALECT: Thumb-2 comes from CONFIG_THUMB=1 in config_win32_arm.h, NOT from a
# flag here. ffmpeg's libavutil/arm/asm.S emits .thumb/.thumb_func from that
# macro. Do not try to force it with -mthumb; the macro layer is authoritative,
# and R4 (no A32 on Windows RT) depends on it.
# -----------------------------------------------------------------------------
set -e

OUT=""
SRC=""
ARGS=""

while [ $# -gt 0 ]; do
  case "$1" in
    -o)
      shift
      OUT="$1"
      ;;
    -o*)
      OUT="${1#-o}"
      ;;
    -c)
      : # implied; clang-cl gets its own -c below
      ;;
    -D*|-I*|-U*)
      ARGS="$ARGS $1"
      ;;
    *.S|*.s)
      SRC="$1"
      ;;
    *)
      : # drop anything else (stray ASFLAGS, -no-integrated-as, ...)
      ;;
  esac
  shift
done

if [ -z "$SRC" ] || [ -z "$OUT" ]; then
  echo "ffvpx-asm-wrap: need a source and -o <obj>; got SRC='$SRC' OUT='$OUT'" >&2
  exit 1
fi

# CC carries the target triple and -fuse-ld from the mozconfig; fall back to a
# bare clang-cl with the triple spelled out if it is somehow unset.
CLANG="${CC:-clang-cl --target=thumbv7-unknown-windows-msvc}"

exec $CLANG -nologo -c -x assembler-with-cpp $ARGS "$SRC" -Fo"$OUT"
