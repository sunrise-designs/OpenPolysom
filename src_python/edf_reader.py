"""Reads ESP32-C6 wrist-logger EDF+ recordings.

Signal layout (label, rate) matches the SigDef table in
ESP32-C6-heart-idf/components/logger/logger.cpp exactly — this module
must be kept in sync with that table if the firmware's channel layout changes.
"""
from datetime import datetime

import edfio
import numpy as np

SIGNAL_LABELS = (
    "Thoracic", "Abdomen", "Flow", "ECG",
    "Accel0X", "Accel0Y", "Accel0Z",
    "Accel1X", "Accel1Y", "Accel1Z",
    "RR",
)

# Signals added to the firmware after the original 11-channel layout. A
# recording made before they existed simply lacks them, so they are read when
# present and left out of `EdfRecording.signals` otherwise — callers test with
# `label in recording.signals`, same as for any other optional channel.
OPTIONAL_SIGNAL_LABELS = (
    "Temp0", "Temp1",  # TMP117 at 0x48 / 0x49, degC, 1 Hz
)

# Digital value that means "no reading" rather than a measurement, per label.
# The firmware writes TEMP_DIGITAL_NO_READING (logger.h) — the declared digital
# minimum, -25600 = -200 degC — when a TMP117 is absent or a read failed,
# because 0 degC is a real temperature and cannot double as "missing". Those
# samples come back as NaN here, so a dropout renders as a gap instead of a
# plunge to -200 degC, and nothing downstream averages it in.
NO_READING_DIGITAL = {
    "Temp0": -25600,
    "Temp1": -25600,
}


class EdfRecording:
    """Physical-unit signal arrays + per-signal sample rates from one EDF+ file."""

    def __init__(self, signals, sample_rates, start_datetime):
        self.signals = signals
        self.sample_rates = sample_rates
        self.start_datetime = start_datetime

    def __getitem__(self, label):
        return self.signals[label]

    def rate(self, label):
        return self.sample_rates[label]


def _physical(sig, label):
    """The signal's physical-unit samples, with any no-reading marker as NaN."""
    data = sig.data
    marker = NO_READING_DIGITAL.get(label)
    if marker is None:
        return data
    data = np.asarray(data, dtype=np.float64).copy()
    data[np.asarray(sig.digital) == marker] = np.nan
    return data


def load_edf(path):
    """Load an ESP32-C6 logger EDF+ file, returning physical-unit signal arrays.

    Each signal is read via edfio's physical (calibrated) path, so values are
    already in the units declared by the EDF+ header (nH, mbar, mg, ms, ADC
    counts, degC) rather than raw digital codes. `OPTIONAL_SIGNAL_LABELS` are
    included only when the file carries them.
    """
    edf = edfio.read_edf(str(path))
    present = {s.label for s in edf.signals}

    signals = {}
    sample_rates = {}
    for label in SIGNAL_LABELS + tuple(l for l in OPTIONAL_SIGNAL_LABELS if l in present):
        sig = edf.get_signal(label)
        signals[label] = _physical(sig, label)
        sample_rates[label] = sig.sampling_frequency

    start_datetime = datetime.combine(edf.startdate, edf.starttime)

    return EdfRecording(signals, sample_rates, start_datetime)
