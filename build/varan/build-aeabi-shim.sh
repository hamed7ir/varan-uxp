#!/bin/sh
# Varan -- rebuild the ARM EABI divmod shim (aeabi-shim.c -> .obj -> .lib).
# The .obj/.lib are committed binaries linked via mozconfig-arm32 LDFLAGS; there
# was no tracked recipe (M1 hand-built them), so any edit to aeabi-shim.c would
# otherwise silently link the stale .lib. Run from any shell with LLVM 18 on the
# hardcoded path below. Uses relative filenames (cd first) to dodge MSYS/Git-Bash
# path-mangling of clang-cl's -Fo D:/... flag.
set -e
# Location-independent: resolve relative to THIS script, not a hardcoded
# working-tree path. The original did `cd /d/repo/mozbuild`, which is why this
# recipe could not travel with the source it builds.
cd "$(dirname "$0")"
CLANGCL="/c/Program Files/LLVM/bin/clang-cl"
LLVMLIB="/c/Program Files/LLVM/bin/llvm-lib"
NM="/c/Program Files/LLVM/bin/llvm-nm"
OBJDUMP="/c/Program Files/LLVM/bin/llvm-objdump"

echo "=== compile aeabi-shim.c -> ARMNT (Thumb-2) ==="
"$CLANGCL" --target=thumbv7-unknown-windows-msvc -O2 -c aeabi-shim.c -Foaeabi-shim.obj
"$LLVMLIB" /OUT:aeabi-shim.lib aeabi-shim.obj

echo "=== symbols (expect T __aeabi_idivmod/uidivmod/idiv/uidiv + U __rt_sdiv/__rt_udiv) ==="
"$NM" aeabi-shim.obj

echo "=== disassembly (expect Thumb-2 swap + tail-call b.w __rt_*div; NO 4-byte A32) ==="
"$OBJDUMP" -d --triple=thumbv7-unknown-windows-msvc aeabi-shim.obj
