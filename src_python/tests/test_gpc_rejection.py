"""Gross Position Change rejection: the amplitude ceiling and the rotation gate.

The distinction under test is the clinical one — a limb jerk leaves the sensor
pointing where it was, a gross position change does not — so the fixtures here
carry a real gravity vector (unlike `_fixtures.make_jerk_signal`, which is
gravity-free) because rotation is only defined against gravity.
"""

import numpy as np
import pytest

from signal_processing import (
    MIN_GRAVITY_MG, count_plm, combine_bilateral_vm, gravity_tilt, gpc_mask,
    remove_baseline,
)

FS = 50
G = 1000.0  # 1 g in mg, the units the MMA8451 reports


def make_gravity_signal(duration_s, fs=FS):
    """A still sensor lying flat: gravity wholly on Z, nothing on X/Y."""
    n = int(round(duration_s * fs))
    return (np.zeros(n), np.zeros(n), np.full(n, G))


def add_jerk(axes, onset_s, duration_s=1.0, amplitude=50.0, fs=FS):
    """A limb movement: a transient on X that leaves orientation untouched."""
    ax, ay, az = (a.copy() for a in axes)
    i0, i1 = int(onset_s * fs), int((onset_s + duration_s) * fs)
    ax[i0:i1] += amplitude
    return ax, ay, az


def add_roll(axes, onset_s, degrees=90.0, duration_s=2.0, amplitude=800.0, fs=FS):
    """A gross position change: gravity rotates from Z toward X and *stays* there,
    with the limb flung about on the way.

    The transient burst is not decoration, it is the whole difficulty. A rolling
    median passes a step edge through untouched, so the orientation change alone
    leaves almost no residual and would never be mistaken for an LM. What makes a
    GPC masquerade as a giant limb movement is the acceleration needed to rotate
    the limb — non-monotonic, so the median does not absorb it. Real GPCs in
    `biometric_2026-07-16_23-00-00.zarr` peak at 900–1300 mg over 6–8 s; a
    fixture without the burst would test nothing.
    """
    ax, ay, az = (a.copy() for a in axes)
    i0, i1 = int(onset_s * fs), int((onset_s + duration_s) * fs)
    theta = np.radians(degrees)
    ramp = np.linspace(0.0, theta, i1 - i0)
    ax[i0:i1] = G * np.sin(ramp)
    az[i0:i1] = G * np.cos(ramp)
    ax[i1:] = G * np.sin(theta)
    az[i1:] = G * np.cos(theta)
    # Rides on Y, orthogonal to the plane of rotation, so the fixture's two
    # effects — the orientation change and the transient — stay separable.
    ay[i0:i1] += amplitude * np.sin(np.linspace(0.0, 2 * np.pi, i1 - i0))
    return ax, ay, az


# ── remove_baseline ────────────────────────────────────────────────────────────

def test_remove_baseline_accepts_a_one_shot_iterable():
    """`channels` is documented as any iterable, and splitting the rolling median
    out into `gravity_baseline` made this function walk it twice — a generator
    then exhausts on the first pass and the result silently comes back empty."""
    axes = make_gravity_signal(60)
    from_list = remove_baseline(list(axes), fs=FS)
    from_generator = remove_baseline((a for a in axes), fs=FS)

    assert len(from_generator) == len(from_list) == 3
    for got, want in zip(from_generator, from_list):
        np.testing.assert_allclose(got, want)


# ── gravity_tilt ───────────────────────────────────────────────────────────────

def test_tilt_is_zero_for_a_still_sensor():
    tilt = gravity_tilt(*make_gravity_signal(300))
    assert tilt.max() < 1e-6


def test_tilt_is_zero_through_a_jerk_that_does_not_rotate_the_sensor():
    """The whole point: amplitude alone must not read as rotation."""
    axes = add_jerk(make_gravity_signal(300), onset_s=150, amplitude=800.0)
    assert gravity_tilt(*axes).max() < 1.0


def test_tilt_recovers_the_roll_angle():
    axes = add_roll(make_gravity_signal(300), onset_s=150, degrees=90.0)
    assert gravity_tilt(*axes).max() == pytest.approx(90.0, abs=2.0)


def test_tilt_is_zero_without_gravity_so_detection_self_disables():
    """Already-baseline-removed input has no gravity vector, so orientation is
    undefined — it must read 0°, not the 90° an unguarded 0/0 arccos gives."""
    n = 300 * FS
    zeros = np.zeros(n)
    assert gravity_tilt(zeros, zeros, zeros).max() == 0.0

    weak = np.full(n, MIN_GRAVITY_MG - 1.0)
    assert gravity_tilt(weak, zeros, zeros).max() == 0.0


# ── the rotation gate ──────────────────────────────────────────────────────────

def test_jerk_is_scored_and_roll_is_not():
    """`max_threshold=None` throughout, so only the rotation gate can reject the
    roll — otherwise the ceiling would catch it and this would prove nothing."""
    jerk = add_jerk(make_gravity_signal(300), onset_s=150, amplitude=50.0)
    assert count_plm(*jerk, threshold=30, fs=FS, max_threshold=None)['total_lms'] == 1

    roll = add_roll(make_gravity_signal(300), onset_s=150, degrees=90.0)
    scored = count_plm(*roll, threshold=30, fs=FS, max_threshold=None)
    assert scored['total_lms'] == 0
    assert scored['total_gpcs'] >= 1


def test_gpc_suppresses_the_lms_it_drafts_in_alongside_it():
    """A roll's baseline step throws off spurious threshold crossings either side
    of itself. Those are part of the position change, not limb movements."""
    axes = add_roll(make_gravity_signal(300), onset_s=150, degrees=90.0)
    axes = add_jerk(axes, onset_s=148, amplitude=60.0)   # just before the roll
    axes = add_jerk(axes, onset_s=153, amplitude=60.0)   # just after it
    assert count_plm(*axes, threshold=30, fs=FS)['total_lms'] == 0


def test_a_jerk_far_from_the_roll_still_scores():
    """The GPC span must not swallow the whole recording."""
    axes = add_roll(make_gravity_signal(600), onset_s=150, degrees=90.0)
    axes = add_jerk(axes, onset_s=400, amplitude=60.0)
    assert count_plm(*axes, threshold=30, fs=FS)['total_lms'] == 1


def test_tilt_threshold_none_disables_the_rotation_gate():
    roll = add_roll(make_gravity_signal(300), onset_s=150, degrees=90.0)
    assert count_plm(*roll, threshold=30, fs=FS,
                     tilt_threshold_deg=None, max_threshold=None)['total_lms'] >= 1


# ── the amplitude ceiling ──────────────────────────────────────────────────────

def test_amplitude_ceiling_rejects_a_violent_transient_that_never_rotates():
    """The case the rotation gate cannot see: a device knock or the unit coming
    off — huge acceleration, no lasting change of orientation."""
    axes = add_jerk(make_gravity_signal(300), onset_s=150, amplitude=900.0)

    assert count_plm(*axes, threshold=30, fs=FS, max_threshold=None)['total_lms'] == 1

    gated = count_plm(*axes, threshold=30, fs=FS, max_threshold=500.0)
    assert gated['total_lms'] == 0
    assert gated['total_gpcs'] == 1


def test_ceiling_keeps_a_movement_at_the_boundary():
    axes = add_jerk(make_gravity_signal(300), onset_s=150, amplitude=100.0)
    assert count_plm(*axes, threshold=30, fs=FS, max_threshold=500.0)['total_lms'] == 1


def test_rejected_movements_are_reported_not_dropped_silently():
    axes = add_jerk(make_gravity_signal(300), onset_s=150, amplitude=900.0)
    result = count_plm(*axes, threshold=30, fs=FS, max_threshold=500.0)
    (on, off), = result['gpc_events']
    assert on == pytest.approx(150.0, abs=0.5)
    assert off > on


# ── invariants ─────────────────────────────────────────────────────────────────

def test_gating_can_only_remove_lms_never_add_them():
    rng = np.random.default_rng(20260717)
    axes = make_gravity_signal(600)
    axes = (axes[0] + rng.normal(0, 5, len(axes[0])),
            axes[1] + rng.normal(0, 5, len(axes[1])),
            axes[2] + rng.normal(0, 5, len(axes[2])))
    for onset in (100, 160, 220, 280):
        axes = add_jerk(axes, onset_s=onset, amplitude=60.0)
    axes = add_roll(axes, onset_s=400, degrees=90.0)

    ungated = count_plm(*axes, threshold=30, fs=FS,
                        max_threshold=None, tilt_threshold_deg=None)
    gated = count_plm(*axes, threshold=30, fs=FS)

    assert gated['total_lms'] <= ungated['total_lms']
    assert gated['total_lms'] + gated['total_gpcs'] == ungated['total_lms']
    assert set(gated['lm_events']).issubset(set(ungated['lm_events']))


# ── the bilateral combination ──────────────────────────────────────────────────

def test_bilateral_unions_both_legs_gpc_masks():
    """A whole-body roll need only rotate one sensor to have thrown the other
    limb around too, and the combined trace is an elementwise max of the pair —
    so a rotation on either leg must gate it."""
    still = make_gravity_signal(300)
    rolled = add_roll(still, onset_s=150, degrees=90.0)
    jerked = add_jerk(still, onset_s=150, amplitude=60.0)

    r_roll = count_plm(*rolled, threshold=30, fs=FS)
    r_jerk = count_plm(*jerked, threshold=30, fs=FS)

    # Leg 1 alone looks like a clean limb movement.
    assert r_jerk['total_lms'] == 1

    combined = combine_bilateral_vm(r_roll['vm'], r_jerk['vm'], threshold=30, fs=FS,
                                    gpc0=r_roll['gpc'], gpc1=r_jerk['gpc'])
    assert combined['total_lms'] == 0

    # Without leg 0's mask the same movement survives — proving the union, and
    # not merely the ceiling, is what rejected it.
    ungated = combine_bilateral_vm(r_roll['vm'], r_jerk['vm'], threshold=30, fs=FS,
                                   gpc1=r_jerk['gpc'])
    assert ungated['total_lms'] >= 1


def test_bilateral_masks_truncate_with_the_traces():
    """A length mismatch must not slide a mask out of step with its trace."""
    still = make_gravity_signal(300)
    rolled = add_roll(still, onset_s=150, degrees=90.0)
    jerked = add_jerk(make_gravity_signal(280), onset_s=150, amplitude=60.0)

    r0 = count_plm(*rolled, threshold=30, fs=FS)
    r1 = count_plm(*jerked, threshold=30, fs=FS)

    combined = combine_bilateral_vm(r0['vm'], r1['vm'], threshold=30, fs=FS,
                                    gpc0=r0['gpc'], gpc1=r1['gpc'])
    assert combined['total_hours'] == pytest.approx(280 / 3600, rel=1e-9)
    assert combined['total_lms'] == 0


def test_gravity_free_input_leaves_bilateral_scoring_untouched():
    """The pre-filtered/synthetic path: no gravity, so the rotation gate is a
    no-op and scoring matches the ungated result exactly."""
    from _fixtures import make_jerk_signal
    ax, ay, az = make_jerk_signal(600, FS, onsets_s=[100, 160, 220, 280])

    assert gpc_mask(ax, ay, az, fs=FS).any() == False  # noqa: E712 — assert the mask, not truthiness

    gated = count_plm(ax, ay, az, threshold=8, fs=FS)
    ungated = count_plm(ax, ay, az, threshold=8, fs=FS,
                        max_threshold=None, tilt_threshold_deg=None)
    assert gated['total_lms'] == ungated['total_lms'] == 4
    assert gated['total_plms'] == ungated['total_plms'] == 4
