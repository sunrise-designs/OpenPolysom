# ProtoSom Agent Rules

## Read the Wiki Before Making Code Changes
Before making any code changes, proposing designs, or executing tasks in this repository, you **MUST** read the ProtoSom Wiki/Knowledgebase.

1. **Start with the Index**: Read the [Wiki Index](file:///c:/Repos/ProtoSom/wiki/INDEX.md) to understand the map of documentation.
2. **Review Crucial Context**:
   - [Component-2 Architecture](file:///c:/Repos/ProtoSom/wiki/knowledge/architecture.md) — The four-stage pipeline, three-layer data model, and language boundary.
   - [Wiki & Session Conventions](file:///c:/Repos/ProtoSom/wiki/standards/conventions.md) — Guidelines on keeping the wiki current.
   - [Coding Standards](file:///c:/Repos/ProtoSom/wiki/standards/coding.md) — Per-language standards, TDD, strict linters, and verification checks.
   - [Data Formats](file:///c:/Repos/ProtoSom/wiki/knowledge/data-formats.md) — Raw anchor details, Zarr v2 specifications, and JSON schemas.
3. **Respect Core Architectural Boundaries**:
   - **C++ ingests, Python processes, TypeScript presents**. The three meet at the Zarr store + metadata.
   - **The TS web app ONLY reads** Zarr and metadata; it **never writes** Zarr.
   - **The Zarr Boundary Contract**: Must use Zarr v2, Blosc (zstd, shuffle) codec, and **no Python-only `numcodecs` filters** (to maintain cross-language readability).
