"""Reads ESP32-C6 wrist-logger EDF+ recordings.

Signal layout (label, rate) matches the SigDef table in
ESP32-C6-heart-idf/components/logger/logger.cpp:292-304 exactly — this module
must be kept in sync with that table if the firmware's channel layout changes.
"""
from datetime import datetime
from pathlib import Path

import edfio

SIGNAL_LABELS = (
    "Thoracic", "Abdomen", "Flow", "ECG",
    "Accel0X", "Accel0Y", "Accel0Z",
    "Accel1X", "Accel1Y", "Accel1Z",
    "RR",
)


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


def load_edf(path):
    """Load an ESP32-C6 logger EDF+ file, returning physical-unit signal arrays.

    Each signal is read via edfio's physical (calibrated) path, so values are
    already in the units declared by the EDF+ header (nH, mbar, mg, ms, ADC
    counts) rather than raw digital codes.
    """
    edf = edfio.read_edf(str(path))

    signals = {}
    sample_rates = {}
    for label in SIGNAL_LABELS:
        sig = edf.get_signal(label)
        signals[label] = sig.data
        sample_rates[label] = sig.sampling_frequency

    start_datetime = datetime.combine(edf.startdate, edf.starttime)

    return EdfRecording(signals, sample_rates, start_datetime)
