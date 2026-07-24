#!/bin/sh
# -----------------------------------------------------------------------------
# Varan (M4.1): Windows-ARM32 libffi assembler wrapper.
#
# Scoped AS override for THIS directory only (set in Makefile.in for
# FFI_TARGET==ARM && OS_TARGET==WINNT). Mirrors the upstream msvcc.sh AS-override
# seam (Bug 1299959) but drives armasm.exe instead of x86 ml/ml64, so the
# Microsoft MSVC-ABI backend (sysv_msvc_arm32.S, armasm syntax) assembles to
# native Thumb-2 -- the ONE file that must not go through clang's integrated-as.
#
# Invoked by config/rules.mk SSRCS rule (~line 874):
#     $(AS) -o $@ $(DEFINES) $(ASFLAGS) $(<name>_FLAGS) $(LOCAL_INCLUDES) -c $<
# We do the two-command dance proven in m4-device/libffi-derisk/DERISK-RESULT.md:
#     1) clang-cl -nologo -EP <-D...> <-I...> src.S   > src.asm   (preprocess)
#     2) armasm  -nologo -o out.obj  src.asm                      (assemble)
#
# Self-contained: derives the MSVC tools bin from INCLUDE (mozbuild sets INCLUDE
# for clang-cl regardless) and bakes msvcdis140.dll onto PATH, so a clean
# checkout builds with ZERO manual PATH surgery (Varan-source one-command build).
# -----------------------------------------------------------------------------
set -e

OUT=""
SRC=""
PPFLAGS=""

# Parse the recipe args: keep -o<obj>, -D*, -I*; find the .S/.s/.asm source;
# ignore everything else (-c, -clang-cl, -no-integrated-as, stray ASFLAGS).
while [ $# -gt 0 ]; do
  case "$1" in
    -o)  OUT="$2"; shift 2 ;;
    -o*) OUT="${1#-o}"; shift ;;
    -D*) PPFLAGS="$PPFLAGS $1"; shift ;;
    -I*) PPFLAGS="$PPFLAGS $1"; shift ;;
    -I)  PPFLAGS="$PPFLAGS -I$2"; shift 2 ;;
    *.S|*.s|*.asm) SRC="$1"; shift ;;
    *)   shift ;;   # -c, -clang-cl, -no-integrated-as, etc. -> not for armasm
  esac
done

if [ -z "$OUT" ] || [ -z "$SRC" ]; then
  echo "armasm-wrap: missing -o <obj> or source (.S). args parsed: OUT='$OUT' SRC='$SRC'" 1>&2
  exit 2
fi

# --- Derive MSVC Hostx64/{arm,x64} from the ...\VC\Tools\MSVC\<ver>\include entry
#     already present in INCLUDE. VC_TOOLS_ARM_BIN overrides if ever needed. ---
ARM_BIN=""
X64_BIN=""
if [ -n "$VC_TOOLS_ARM_BIN" ]; then
  ARM_BIN="$VC_TOOLS_ARM_BIN/arm"
  X64_BIN="$VC_TOOLS_ARM_BIN/x64"
else
  OLDIFS="$IFS"; IFS=';'
  for e in $INCLUDE; do
    # normalize backslashes to forward slashes, strip trailing slash
    n=$(printf '%s' "$e" | tr '\\' '/' | sed 's:/*$::')
    case "$n" in
      */[Vv][Cc]/[Tt]ools/[Mm][Ss][Vv][Cc]/*/include|*/VC/Tools/MSVC/*/include)
        ver_dir=$(dirname "$n")            # ...\MSVC\<ver>
        ARM_BIN="$ver_dir/bin/Hostx64/arm"
        X64_BIN="$ver_dir/bin/Hostx64/x64"
        break ;;
    esac
  done
  IFS="$OLDIFS"
fi
if [ -z "$ARM_BIN" ]; then
  echo "armasm-wrap: could not derive MSVC tools bin from INCLUDE; set VC_TOOLS_ARM_BIN=...\\MSVC\\<ver>\\bin\\Hostx64" 1>&2
  exit 2
fi
# The build recipe runs under MozillaBuild's MSYS sh, so PATH entries must be
# POSIX form ("/d/..."), NOT Windows-drive form ("D:/...") -- the ':' in a drive
# letter is the MSYS PATH separator and would split the entry. Convert.
to_posix() {
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -u "$1"
  else
    printf '%s' "$1" | sed -E 's|\\|/|g; s|^([A-Za-z]):|/\l\1|'
  fi
}
# msvcdis140.dll (x64) + armasm (arm) onto PATH (POSIX form for MSYS).
PATH="$(to_posix "$X64_BIN"):$(to_posix "$ARM_BIN"):$PATH"
export PATH

PPASM="${OUT%.*}.asm"

# STEP 1: preprocess (clang-cl -EP; no #line markers) -> armasm-syntax .asm
clang-cl -nologo -EP $PPFLAGS "$SRC" > "$PPASM"

# STEP 2: armasm -> Thumb-2 COFF-ARM object (armasm uses `-o obj src`, NOT -c)
"$ARM_BIN/armasm.exe" -nologo -o "$OUT" "$PPASM"
