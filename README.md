# varan-uxp — UXP for ARM32 Windows RT

The **platform** half of [Varan](https://github.com/hamed7ir/varan): the Unified
XUL Platform (Goanna 6.9.0) ported to **32-bit ARM Windows RT** (Surface RT /
Surface 2, Tegra 3 / Cortex-A9). Upstream UXP does not target this platform.

> Not affiliated with or endorsed by Moonchild Productions. Report bugs here,
> never to the Pale Moon or UXP projects.

## Provenance

| | |
|---|---|
| upstream | `https://repo.palemoon.org/MoonchildProductions/UXP` |
| fork point | `81fce269f524c16cca7c3ce84b66002ce9b7da8b` — UXP `RB_20260914` |
| application | [varan](https://github.com/hamed7ir/varan), fork point `fcd973b3` (Pale Moon 35.0.0_Release) |

GitHub cannot draw a fork relationship to a Gitea-hosted upstream, so the fork
point is recorded here. `git merge-base HEAD 81fce269` should return `81fce269`
exactly; if it does not, this README is stale.

The two fork points are the **exact pair** — upstream `fcd973b3`'s `platform`
gitlink is `81fce269`.

## What the port actually required

Not a recompile. The substantive work, roughly in order of difficulty:

- **A from-scratch Thumb-2 JIT backend.** SpiderMonkey's `jit/arm` is A32-only,
  and sustained A32 is device-lethal here — Windows RT drops `CPSR.T` across
  preemption, so an A32 stub passes every build check and fails *intermittently*
  on hardware. Everything that enters the build must be Thumb-2, verified by
  disassembly rather than by "it linked".
- **Toolchain codegen bugs.** Six confirmed `clang-cl` / `lld`
  `thumbv7-windows-msvc` defects, several device-lethal, worked around per-site
  and in one case by a link-time COMDAT override. They are catalogued in the
  build tree; two of them corrupt argument registers or clear the Thumb bit on
  function pointers, which is why this port carries post-link fixups and gates.
- **js-ctypes / libffi.** Vendored libffi 3.4.6 for its Microsoft-contributed
  Windows-ARM32 MSVC-ABI backend. Without ctypes, `OS.File` never loads and
  Places, search, session store and the address bar are all dead.
- **Audio, media SIMD, and the compositor**, each a separate device-proven fix.

## "VENICE" in the comments

**VENICE is the test device** — a Surface RT (Tegra 3, 4×Cortex-A9, Windows RT
8.1). Comments name it when recording something proven *on real silicon* rather
than reasoned about. `"VENICE's I-cache is device-proven NOT auto-coherent"`
means someone measured it, and in a port like this the difference between
measured and assumed is the difference between a fix and a guess. Kept
deliberately.

## Licence

Mozilla Public License 2.0, as upstream. Modifications are disclosed by the git
history: Varan commits are prefixed `Varan:`, and the full diff against
`81fce269` is the complete set of changes. Upstream copyright notices are
preserved.

## Diagnostics shipped in release builds

`layout/base/VaranPhases.cpp` — six-phase main-thread accounting (style, reflow,
paint, JS, GC, CC). Compiled in, and **inert unless** `VARAN_PHASES` is set in
the environment; it writes nothing when off. Disclosed here rather than left to
be discovered.

---

# Building

`varan-uxp` is the **platform** half and is not built on its own. It is consumed
as the `platform/` submodule of [varan](https://github.com/hamed7ir/varan).

**See [varan's README](https://github.com/hamed7ir/varan#building) for the full
build instructions** — toolchain, prerequisites, mozconfig, gates and device
deployment all live there, because the build runs from the application tree.

## What lives here that the build needs

`build/varan/` carries three files the build cannot proceed without, and which
were external to this repository until 2026-08-02 (so the fork could not be
built from a clean clone):

| file | what it is |
|---|---|
| `aeabi-shim.c` | `__aeabi_idivmod` / `__aeabi_uidivmod` wrappers over the MSVC ARM CRT. Tegra 3 has **no hardware integer divide**, and where SpiderMonkey names the combined EABI helpers explicitly, clang emits calls the MSVC ARM CRT does not ship. |
| `build-aeabi-shim.sh` | rebuilds `aeabi-shim.lib` from the above. Run it if you touch the `.c`, or the stale `.lib` links silently. |
| `arm-winnt-shim.h` | force-included before the Windows SDK headers. clang-cl 18's `<armintr.h>` defines only the `_ARM_BARRIER_*` enum, **not** the CP15 coprocessor-register macros that the SDK's `um/winnt.h` passes to `_MoveFromCoprocessor()`. MSVC's own `armintr.h` has them; this supplies them. |
