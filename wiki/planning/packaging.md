---
title: Cross-platform Air-gapped Packaging
domain: planning
status: snapshot
updated: 2026-06-19
summary: How the three-language stack ships as air-gapped Windows/Linux/Mac installers, and the per-OS costs.
---

# Packaging

**Goal:** install and run with **no internet** (air-gapped) on Windows, Linux, and Mac. Three languages make this the heaviest case, but each part is individually feasible and well-trodden.

## The three runtimes

- **C++ ingest/export** → compiled binaries, bundled per-OS. Cheapest to ship.
- **Python processing** → a [conda `constructor`](pipeline-assessment.md) installer (pin **conda-forge**, *not* the Anaconda `defaults` channel — commercial-use trap), or a vendored wheelhouse (`pip install --no-index --find-links`). numpy/scipy native extensions are the classic packaging pain; conda handles them cleanly.
- **TS web app** → a prebuilt static bundle (esbuild, committed) + a thin [slicing server](../knowledge/viewer.md). The server ships either as Node, or as a single self-contained binary via `bun build --compile` / `deno compile` / Node SEA. No Node runtime is needed just to *view* (the bundle is static).

## Per-OS costs (unavoidable)

- **No cross-compilation** of native stacks → one build host / CI runner per OS.
- **macOS Gatekeeper:** Apple Developer ID + notarization ($99/yr) is needed only for a frictionless *web-download* double-click. An air-gapped or source-built app runs fine after `xattr -cr`.
- **Windows:** if any Python is frozen (PyInstaller), use **onedir**, never onefile, to cut antivirus false positives.

See [pipeline-assessment](pipeline-assessment.md) (the packaging answer) and [roadmap](../state/roadmap.md) (stage 8). Packaging is the last build stage — it follows the rest of the pipeline working.
