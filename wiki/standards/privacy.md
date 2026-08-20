---
title: Privacy & PII Handling
domain: standards
status: living
updated: 2026-08-20
summary: How patient-identifying data is kept out of the public repo, scrubbed from shareable exports, and kept off the air when the device streams live.
---

# Privacy & PII

ProtoSom is a **public** repo. Patient-identifying data must never be committed, and must be kept **separable** so it can be scrubbed from anything shared.

## Current state

Two mechanisms keep generated PII out of the tree:

1. **Pipeline outputs are written outside the repo tree.** `read_log.py` writes the
   `.zarr` working store and its `_meta.json` / `events.json` sidecars into
   **`data_scratchpad/`** (`export_zarr.DEFAULT_OUT_DIR`, overridable with `--out-dir`),
   which is gitignored whole. `serve.py`, `deploy.py` and `serve_metrics.py` resolve
   recordings from there via `export_zarr.find_meta`. A tool that needs a new output must
   take an `out_dir` rather than defaulting to the cwd — the repo root is where
   [`how to use.md`](../../how%20to%20use.md) tells the user to run everything from.
2. **The gitignore rules match shapes, not exact filenames.** `/data_scratchpad/`,
   `*_meta.json` and `/events.json`, alongside the existing `patient.cfg`,
   `src_python/patient.json` and `src_python/netlify.json` exclusions. **Never narrow
   these back to literal filenames** — an exact-name rule does not cover a *timestamped*
   `biometric_<date>_meta.json`.

Note that `*.edf` is **not** gitignored and the recordings under `examples/` are tracked
deliberately. That is acceptable only because the device's EDF+ header is PII-free by
construction (below) — it is not a general licence to commit recordings.

## Where PII appears in the design

- The device **EDF+ header carries no PII.** The ESP32-C6 firmware calls neither
  `edf_set_patientname` nor `edf_set_birthdate` — there is no such call anywhere in
  `ESP32-C6-heart-idf`. The raw anchor is therefore **PII-free by construction**, so there is no
  conflict between "the audit anchor is immutable and hashed" and "the anchor embeds a patient's
  name". Nothing in the capture path needs scrubbing.
- `meta.json` may hold a patient block (name / DOB / NHS number / email) — this is now the **only**
  place PII enters the pipeline, and it is added host-side, not by the device.
- `src_python/patient.json` — a local, gitignored file (`name`/`dob`/`nhs_number`/`email`) read by `_load_patient()` in `export_zarr.py`, `plotting_html.py`, and `plotting_pdf.py`, and merged into `meta.json`'s `subject.pii` block and the HTML/PDF report headers.

## `patient.json` — real PII, local-only

- `src_python/patient.json` is **git-ignored by exact filename** (`src_python/patient.json` in `.gitignore`) and is never committed. Each machine keeps its own copy locally.
- `src_python/patient.example.json` is a **committable stub** with mock values (`Jane Doe`, etc.) matching the same shape, so a fresh checkout shows the expected fields without ever holding real PII. It carries an `_instructions` key telling the user to copy it to `patient.json` and fill in the real values. It is **not** matched by any gitignore rule (only the exact `patient.json` filename is), so it commits and tracks normally.
- **Missing-file behaviour is explicit, not silent.** If `patient.json` isn't present, `_load_patient()` in each of the three consumers prints a `[patient] ... not found` message naming the exact thing that will be omitted (`subject.pii` in `meta.json`, the HTML report title, or the PDF report header) and points at `patient.example.json` — then returns `None` and the pipeline continues without patient info. This is a deliberate degrade-gracefully design, not an error: a recording with no `patient.json` must still process and export successfully (PII stays optional so the working store and exports can be shared).

## Live streaming over the air

The device's RT stream ([decisions § S12](../state/decisions.md)) puts physiological
signals on a radio link. Two things keep that acceptable:

- **No identifiers are streamed.** The frames carry channel samples plus a
  MAC-derived `device_uid`, and nothing else. PII enters the pipeline host-side, in
  `meta.json` — the device has none to leak (see above).
- **The access point is WPA2, never open.** The password is derived per-device from
  the MAC and shown on the OLED, so it is discoverable to whoever is holding the unit
  and not to a passer-by. An open AP would put a live physiological trace in range of
  any phone in the room; that is the specific thing this prevents. It is not a strong
  secret and is not treated as one — the point is that the default is not "broadcast
  in the clear".

SoftAP is also the default *because* of this: the stream stays on a link between the
device and the one machine watching it, rather than transiting a home LAN.

## Rules

1. **Keep PII in a separable block** — a single `patient { … }` object in `meta.json`, never inlined into signal arrays or the [working store](../knowledge/data-formats.md). This lets the working store and exports be shared without it.
2. **The clinical export scrubs it.** The Zarr → EDF/BDF [clinical export](../knowledge/data-formats.md) blanks the EDF+ header patient name/DOB to produce a de-identified, shareable `clinical.edf`. Since the device no longer writes PII into the header, the export's job is to avoid *introducing* PII from `meta.json` — not to remove PII inherited from the capture. Keep the scrubbing anyway: it costs nothing and guards against a future producer that does populate the header.
3. **Pipeline outputs stay in `data_scratchpad/`.** Anything a processing run generates — the working store, `meta.json`, `events.json`, the HTML/PDF reports — is written there and never into the repo tree. A tool that needs a new output must take an `out_dir`, not default to the cwd.
4. **Never relax the gitignore** for `patient.cfg`, `src_python/patient.json`, `/data_scratchpad/`, `*_meta.json`, or any `meta.json` that contains PII. `src_python/patient.example.json` is deliberately a different filename so it's untouched by those rules and commits normally — it must never hold anything but mock data.

See [decisions](../state/decisions.md) — settled item S8 (export scrubbing) and open fork O9 (the de-identification policy still to finalise with Leon/Dmitry).
