---
title: ProtoSom Wiki — Index
domain: knowledge
status: living
updated: 2026-06-19
summary: Root entry point for the ProtoSom LLM wiki — read this first, then open only the pages you need.
---

# ProtoSom — LLM wiki

ProtoSom is an open-source proof-of-concept **polysomnography** (sleep study) system: it records breathing (RIP belts + nasal flow), heart (ECG/RR, HRV), movement, and snoring audio over a night, to make sleep-disorder screening accessible and affordable. It aspires *voluntarily* to IEC 62304 medical-software discipline (traceability, tests, reproducibility); it is **not** a regulated medical device.

This wiki is the knowledge base for sessions/agents working on **component 2** — the data pipeline after data leaves the device. **Read this index first, then open only the pages you need.**

## The 30-second model

- **C++ ingests** (device → raw Zarr) · **Python processes** (→ derived Zarr + metadata) · **TypeScript presents** (the web app reads Zarr, never writes it).
- **Zarr + metadata is the boundary** between Dmitry's side (C++ ingest + Python processing) and Leon's side (the TS web app). It is a *language-neutral* contract.
- **Three layers:** raw anchor (EDF+/FLAC, immutable + hashed) → working store (Zarr) → clinical export (EDF/BDF, on demand).

## Map

### `knowledge/` — how it works
- [architecture](knowledge/architecture.md) — the spine: pipeline, 3-layer model, language boundary, the membrane. **Start here.**
- [data-formats](knowledge/data-formats.md) — EDF+/BDF+, FLAC, the Zarr boundary, the JSON sidecars.
- [signal-processing](knowledge/signal-processing.md) — the Python processing: baseline removal, AASM PLM, HRV, and the future roadmap.
- [hardware](knowledge/hardware.md) — the devices, their channels, and the C++ ingest side.
- [viewer](knowledge/viewer.md) — the TS web app + slicing server.
- [concepts](knowledge/concepts.md) — PSG + data-tech glossary.

### `standards/` — how we work
- [conventions](standards/conventions.md) — wiki structure + how a session reads/updates it.
- [coding](standards/coding.md) — per-language coding standards (C++ / Python / TS).
- [privacy](standards/privacy.md) — PII handling + de-identified export.

### `planning/` — point-in-time design (snapshot, may go stale)
- [pipeline-assessment](planning/pipeline-assessment.md) — the architecture assessment that produced these decisions.
- [zarr-schema-spec](planning/zarr-schema-spec.md) — the full PSG-on-Zarr schema specification.
- [packaging](planning/packaging.md) — air-gapped cross-platform packaging.

### `state/` — the current picture (kept current)
- [current](state/current.md) — what exists today vs what's planned.
- [decisions](state/decisions.md) — settled decisions + the open forks.
- [roadmap](state/roadmap.md) — the ordered build plan.

## Where to start building

Read [architecture](knowledge/architecture.md) for the shape, [decisions](state/decisions.md) for what's settled vs open, then [roadmap](state/roadmap.md) for the ordered next steps.
