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
| fork point | `659c690d5b34cc3e46c5ba8a6e00f134d8d20c35` — UXP `RB_20260624` |
| application | [varan](https://github.com/hamed7ir/varan), fork point `0d869b85` (Pale Moon 34.3.1_Release) |

GitHub cannot draw a fork relationship to a Gitea-hosted upstream, so the fork
point is recorded here. `git merge-base HEAD 659c690d` should return `659c690d`
exactly; if it does not, this README is stale.

The two fork points are the **exact pair** — upstream `0d869b85`'s `platform`
gitlink is `659c690d`.

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
`659c690d` is the complete set of changes. Upstream copyright notices are
preserved.

## Diagnostics shipped in release builds

`layout/base/VaranPhases.cpp` — six-phase main-thread accounting (style, reflow,
paint, JS, GC, CC). Compiled in, and **inert unless** `VARAN_PHASES` is set in
the environment; it writes nothing when off. Disclosed here rather than left to
be discovered.
