---
title: Wiki & Session Conventions
domain: standards
status: living
updated: 2026-06-19
summary: How this wiki is structured and how a working session should read and keep it current.
---

# Conventions

## Reading order

Start at [INDEX](../INDEX.md) → [architecture](../knowledge/architecture.md) → then only the pages your task needs. The INDEX is `llms.txt`-shaped on purpose: a map, not a dump.

## Folder roles

- `knowledge/` — **how it works** (durable; changes only when the design changes).
- `standards/` — **how we work** (conventions, coding, privacy).
- `planning/` — **point-in-time** design/snapshots; true as of their `updated` date, expected to go stale.
- `state/` — **the current picture**; kept current as the build progresses.

## Front-matter (every page)

```yaml
---
title: <Page Title>
domain: knowledge | standards | planning | state
status: living | snapshot
updated: YYYY-MM-DD
summary: <one sentence>
---
```

`living` pages are maintained; `snapshot` pages are frozen records. Bump `updated` whenever you change content.

## Canonical terms (use these exact words)

**raw anchor**, **working store**, **derived layer**, **clinical export**, **C++ ingest**, **Python processing**, **the TS web app**, **the slicing server**, **the Zarr boundary**. See [architecture](../knowledge/architecture.md) for what each means.

## Keeping it current (the session's job)

- Changed a decision? → update [decisions](../state/decisions.md) **and** the affected `knowledge/` page, and bump both dates.
- Built or changed something? → update [current](../state/current.md) and tick the step in [roadmap](../state/roadmap.md).
- New durable knowledge? → it belongs in `knowledge/`, not buried in `state/`.

## Links

Use **relative** markdown links and link liberally (e.g. `[architecture](../knowledge/architecture.md)`). A link to a page that doesn't exist yet is a TODO, not an error — but don't invent page names: link to the canonical page that actually holds the content.
