#!/usr/bin/env python3
"""ProtoSom Zarr-boundary fixture generator (dev / test oracle).

This is NOT the production Python processing (that is Dmitry's side). It is a
TypeScript-side dev tool: it writes a **spec-compliant** working store so the TS
web app and its tests have a real, correct instance of the Zarr boundary to read
against — and it doubles as the *writer* oracle for the cross-language
read-parity test (zarr-python writes, zarrita.js reads, values must agree).

It emits, from the committed legacy 6-hour sample (`biometric_filtered.bin`,
3xuint8 accel + uint16 RR @10 Hz):

  <out>/working.zarr   — Zarr v2, Blosc(zstd, shuffle), filters=None,
                         dimension_separator=".", chunked along time, per-array
                         `storage` attr, NaN fill on floats
  <out>/meta.json      — metadata + provenance (full git SHA + dirty, raw-anchor
                         hash, input prep) + study-level stats
  <out>/events.json    — sparse LM events + PLM series group (schema-shaped)

and a tiny deterministic parity store for the Vitest read-parity test.

Run (no venv needed):
  uv run --with zarr --with numcodecs --with numpy --with scipy \
      tools/make_fixture.py
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import zarr
from numcodecs import Blosc

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "src_python"))

from signal_processing import count_plm, compute_hrv  # noqa: E402

FS = 10  # legacy .bin sample rate (Hz)
SKIP_DEFAULT = 700  # leading samples dropped before DSP (read_log default)
COMPRESSOR = Blosc(cname="zstd", clevel=5, shuffle=Blosc.SHUFFLE)
SCHEMA_VERSIONS = {"meta": "0.1.0", "events": "1.0.0", "zarr_arrays": "0.1.0"}


def _git() -> dict:
    def run(*a: str) -> str:
        return subprocess.check_output(["git", *a], cwd=REPO, text=True).strip()

    try:
        sha = run("rev-parse", "HEAD")
        dirty = bool(run("status", "--porcelain"))
        branch = run("rev-parse", "--abbrev-ref", "HEAD")
        return {"sha": sha, "dirty": dirty, "branch": branch}
    except Exception:
        return {"sha": "unknown", "dirty": True, "branch": "unknown"}


def _hash_file(path: Path) -> dict:
    try:
        import blake3  # type: ignore

        h = blake3.blake3(path.read_bytes()).hexdigest()
        return {"algorithm": "blake3", "value": h}
    except Exception:
        h = hashlib.sha256(path.read_bytes()).hexdigest()
        return {"algorithm": "sha256", "value": h}


def _add(group: zarr.Group, name: str, data: np.ndarray, *, attrs: dict) -> None:
    """Create one boundary-compliant v2 array (Blosc, time-chunked, no filters)."""
    is_float = np.issubdtype(data.dtype, np.floating)
    n = int(data.shape[0])
    chunk = (min(n, 36000) or 1,)  # ~1 h at 10 Hz; one chunk if tiny
    # zarr-python 3.x: v2 stores default to dimension_separator="." (the boundary
    # spec value); it is also moot for these 1-D arrays. compressor= (singular)
    # selects the v2 codec; filters=None per the no-Python-only-filters rule.
    arr = group.create_array(
        name=name,
        shape=data.shape,
        chunks=chunk,
        dtype=data.dtype,
        compressor=COMPRESSOR,
        filters=None,
        fill_value=(np.nan if is_float else 0),
        attributes=attrs,
    )
    if n > 0:
        arr[...] = data


def build_working_store(bin_path: Path, out_dir: Path, skip: int) -> dict:
    raw = bin_path.read_bytes()
    anchor_hash = _hash_file(bin_path)  # hash the immutable raw anchor (whole file)

    data = raw[skip * 5:] if skip > 0 else raw
    records = [
        (data[i], data[i + 1], data[i + 2], struct.unpack_from("<H", data, i + 3)[0])
        for i in range(0, len(data) - 4, 5)
    ]
    n = len(records)
    if n == 0:
        raise SystemExit(f"No records parsed from {bin_path} (after skip={skip}).")

    ax = np.array([r[0] for r in records], dtype=np.uint8)
    ay = np.array([r[1] for r in records], dtype=np.uint8)
    az = np.array([r[2] for r in records], dtype=np.uint8)
    rr = np.array([r[3] for r in records], dtype=np.float32)
    t = (np.arange(n, dtype=np.float64) / FS)

    plm = count_plm(data, threshold=8, fs=FS)
    accel_mag = np.asarray(plm["vm"], dtype=np.float32)
    hrv_overall, hrv_t, hrv_rmssd = compute_hrv([r[3] for r in records], fs=FS)
    hrv_t = np.asarray(hrv_t, dtype=np.float64)
    hrv_rmssd = np.asarray(hrv_rmssd, dtype=np.float32)

    duration_s = n / FS
    start_dt = datetime.fromtimestamp(bin_path.stat().st_mtime, tz=timezone.utc)
    t0_iso = start_dt.isoformat()
    recording_id = f"{start_dt.strftime('%Y-%m-%d_%H%M')}_protosom-wrist"

    store_path = out_dir / "working.zarr"
    if store_path.exists():
        import shutil

        shutil.rmtree(store_path)
    g = zarr.open_group(str(store_path), mode="w", zarr_format=2)
    g.attrs.update({
        "protosom_meta": "meta.json",
        "protosom_schema_version": SCHEMA_VERSIONS["zarr_arrays"],
        "recording_id": recording_id,
        "t0_iso": t0_iso,
    })

    _add(g, "t", t, attrs={"protosom_role": "axis", "harmonized_unit": "s",
                           "sample_rate_hz": float(FS), "_ARRAY_DIMENSIONS": ["time"]})
    for nm, d in (("accel_x", ax), ("accel_y", ay), ("accel_z", az)):
        _add(g, nm, d, attrs={"protosom_role": "signal", "storage": "digital",
                              "kind": "accelerometer", "harmonized_unit": "count",
                              "sample_rate_hz": float(FS), "source_device": "esp32-wrist",
                              "_ARRAY_DIMENSIONS": ["time"]})
    _add(g, "rr", rr, attrs={"protosom_role": "signal", "storage": "digital",
                             "kind": "rr_interval", "harmonized_unit": "ms",
                             "sample_rate_hz": float(FS), "source_device": "esp32-wrist",
                             "_ARRAY_DIMENSIONS": ["time"]})
    _add(g, "accel_mag", accel_mag, attrs={"protosom_role": "derived",
                                           "kind": "accelerometer", "harmonized_unit": "count",
                                           "sample_rate_hz": float(FS),
                                           "fill_value_meaning": "gap_or_missing (NaN)",
                                           "_ARRAY_DIMENSIONS": ["time"]})
    _add(g, "hrv_t", hrv_t, attrs={"protosom_role": "axis", "harmonized_unit": "s",
                                   "_ARRAY_DIMENSIONS": ["time"]})
    _add(g, "hrv_rmssd", hrv_rmssd, attrs={"protosom_role": "derived", "kind": "heart_rate",
                                           "harmonized_unit": "ms", "time_array": "hrv_t",
                                           "fill_value_meaning": "gap_or_missing (NaN)",
                                           "_ARRAY_DIMENSIONS": ["time"]})

    git = _git()
    generated_at = datetime.now(tz=timezone.utc).isoformat()
    hrv_val = None if (hrv_overall is None or np.isnan(hrv_overall)) else float(hrv_overall)

    meta = {
        "schema_versions": SCHEMA_VERSIONS,
        "recording_id": recording_id,
        "subject": {"subject_id": "PSG-0001", "sex": None, "dob": None},
        "recording": {"start_iso": t0_iso, "duration_s": duration_s,
                      "n_samples": n, "sample_rate_hz": FS, "source": "esp32-wrist (legacy .bin)"},
        "device": {"channels": [
            {"zarr_array": "accel_x", "kind": "accelerometer", "fs_hz": FS, "storage": "digital", "dtype": "uint8"},
            {"zarr_array": "accel_y", "kind": "accelerometer", "fs_hz": FS, "storage": "digital", "dtype": "uint8"},
            {"zarr_array": "accel_z", "kind": "accelerometer", "fs_hz": FS, "storage": "digital", "dtype": "uint8"},
            {"zarr_array": "rr", "kind": "rr_interval", "fs_hz": FS, "storage": "digital", "dtype": "float32"},
        ]},
        "stats": {
            "plmi": float(plm["plmi"]),
            "total_lms": int(plm["total_lms"]),
            "total_plms": int(plm["total_plms"]),
            "total_hours": float(plm["total_hours"]),
            "hrv_rmssd_overall": hrv_val,
            "threshold": 8.0, "window_sec": 30.0, "fs": FS,
        },
        "layers": {
            "raw": {"biosignals": [{"path": bin_path.name, "format": "legacy-bin-5byte",
                                    "hash": anchor_hash}]},
            "working": {"path": "working.zarr", "zarr_format": 2, "dimension_separator": "."},
        },
        "provenance": {
            "generated_at": generated_at,
            "pipeline": {"repo": "sunrise-designs/ProtoSom", "git": git},
            "input_preparation": {"skip_samples": skip,
                                  "note": f"leading {skip / FS:.1f} s dropped before DSP"},
        },
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=2))

    lm_events = plm["lm_events"]
    id_of = {}
    events = []
    for i, (on, off) in enumerate(lm_events, start=1):
        ev_id = f"ev_{i:04d}"
        id_of[(round(float(on), 4), round(float(off), 4))] = ev_id
        events.append({"id": ev_id, "type": "limb_movement",
                       "onset_s": float(on), "duration_s": float(off) - float(on),
                       "channels": ["accel_mag"]})
    groups = []
    for gi, grp in enumerate(plm["plm_groups"], start=1):
        members = [id_of[(round(float(a), 4), round(float(b), 4))] for a, b in grp]
        on0 = float(grp[0][0])
        off_last = float(grp[-1][1])
        groups.append({"id": f"grp_plm_{gi:03d}", "type": "plm_series",
                       "member_ids": members, "onset_s": on0,
                       "duration_s": off_last - on0,
                       "params": {"n_movements": len(members)}})

    events_doc = {
        "schema": "protosom.events", "schema_version": SCHEMA_VERSIONS["events"],
        "recording_id": recording_id,
        "time_base": {"reference": "recording_start", "start_iso": t0_iso, "unit": "s"},
        "channels_ref": "working.zarr",
        "scorings": [{
            "scoring_id": "plm-auto-v0",
            "scorer": {"kind": "algorithm", "name": "protosom.count_plm", "version": "0.3.1"},
            "scoring_method": "auto",
            "provenance": {"function": "signal_processing.count_plm",
                           "code_version": git["sha"] + (":dirty" if git["dirty"] else ""),
                           "params": {"threshold": 8.0, "fs": FS},
                           "source_constants": {"lm_dur_s": [0.5, 10.0],
                                                "plm_gap_s": [5, 90], "plm_min_series": 4},
                           "input_signals": ["accel_mag"],
                           "raw_refs": [{"path": bin_path.name, "hash": anchor_hash}]},
            "events": events, "groups": groups,
        }],
    }
    (out_dir / "events.json").write_text(json.dumps(events_doc, indent=2))

    return {"recording_id": recording_id, "n": n, "duration_s": duration_s,
            "plmi": float(plm["plmi"]), "total_lms": int(plm["total_lms"]),
            "total_plms": int(plm["total_plms"]), "hrv": hrv_val,
            "out": str(out_dir)}


def build_parity_fixture(path: Path) -> None:
    """Tiny deterministic Blosc-v2 store for the zarrita read-parity test."""
    if path.exists():
        import shutil

        shutil.rmtree(path)
    g = zarr.open_group(str(path), mode="w", zarr_format=2)
    g.attrs.update({"protosom_schema_version": "0.1.0", "purpose": "read-parity-test"})
    # float32 with a NaN to exercise the "NaN" fill/value path
    f = np.array([0.0, 1.5, 2.5, np.nan, 4.0, 5.0, 6.0, 7.0], dtype=np.float32)
    _add(g, "f32", f, attrs={"protosom_role": "derived", "harmonized_unit": "x",
                             "_ARRAY_DIMENSIONS": ["time"]})
    u = np.array([10, 20, 30, 40, 50, 60, 70, 80], dtype=np.uint8)
    _add(g, "u8", u, attrs={"protosom_role": "signal", "storage": "digital",
                            "_ARRAY_DIMENSIONS": ["time"]})


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default=str(REPO / "biometric_filtered.bin"))
    ap.add_argument("--out", default=str(REPO / "src_web" / "public" / "sample"))
    ap.add_argument("--skip", type=int, default=SKIP_DEFAULT)
    ap.add_argument("--parity", default=str(REPO / "src_web" / "test" / "fixtures" / "parity.zarr"))
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    summary = build_working_store(Path(args.bin), out_dir, args.skip)

    parity = Path(args.parity)
    parity.parent.mkdir(parents=True, exist_ok=True)
    build_parity_fixture(parity)

    print(json.dumps(summary, indent=2))
    print(f"parity fixture -> {parity}")


if __name__ == "__main__":
    main()
