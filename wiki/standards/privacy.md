---
title: Privacy & PII Handling
domain: standards
status: living
updated: 2026-07-08
summary: How patient-identifying data is kept out of the public repo and scrubbed from shareable exports.
---

# Privacy & PII

ProtoSom is a **public** repo. Patient-identifying data must never be committed, and must be kept **separable** so it can be scrubbed from anything shared.

## Current state (good)

`.gitignore` already excludes `patient.cfg`, `*.json`, `*.edf`, and `*.csv`. **No real PII is, or ever was, committed.**

## Where PII appears in the design

- The device **EDF+ header** carries patient name + DOB (`src/main.cpp` `edf_set_patientname` / `edf_set_birthdate`).
- `meta.json` may hold a patient block (name / DOB / NHS number / email).

## Rules

1. **Keep PII in a separable block** — a single `patient { … }` object in `meta.json`, never inlined into signal arrays or the [working store](../knowledge/data-formats.md). This lets the working store and exports be shared without it.
2. **The clinical export scrubs it.** The Zarr → EDF/BDF [clinical export](../knowledge/data-formats.md) blanks the EDF+ header patient name/DOB to produce a de-identified, shareable `clinical.edf`.
3. **Never relax the gitignore** for `patient.cfg` or any `meta.json` that contains PII.

See [decisions](../state/decisions.md) — settled item S8 (export scrubbing) and open fork O9 (the de-identification policy still to finalise with Leon/Dmitry).
