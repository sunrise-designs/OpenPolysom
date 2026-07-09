import pytest

from metrics_windowing import filter_lm_events, filter_plm_group_members, pad_window, windowed_hours


def test_pad_window_clips_at_recording_start():
    padded = pad_window(start_s=10.0, end_s=50.0, context_before_s=120.0, context_after_s=120.0,
                         recording_duration_s=600.0)
    assert padded.padded_start_s == 0.0
    assert padded.padded_end_s == 170.0
    assert padded.clipped_to_recording_bounds is True
    # the requested window itself is preserved verbatim, never clipped
    assert padded.start_s == 10.0
    assert padded.end_s == 50.0


def test_pad_window_clips_at_recording_end():
    padded = pad_window(start_s=550.0, end_s=590.0, context_before_s=120.0, context_after_s=120.0,
                         recording_duration_s=600.0)
    assert padded.padded_start_s == 430.0
    assert padded.padded_end_s == 600.0
    assert padded.clipped_to_recording_bounds is True


def test_pad_window_not_clipped_mid_recording():
    padded = pad_window(start_s=300.0, end_s=340.0, context_before_s=120.0, context_after_s=120.0,
                         recording_duration_s=600.0)
    assert padded.padded_start_s == 180.0
    assert padded.padded_end_s == 460.0
    assert padded.clipped_to_recording_bounds is False


def test_filter_lm_events_keeps_only_onsets_in_half_open_window():
    events = [(10.0, 11.0), (20.0, 21.0), (30.0, 31.0)]
    assert filter_lm_events(events, 15.0, 30.0) == [(20.0, 21.0)]
    # end_s is exclusive, start_s is inclusive
    assert filter_lm_events(events, 10.0, 30.0) == [(10.0, 11.0), (20.0, 21.0)]


def test_filter_plm_group_members_flattens_and_filters():
    groups = [[(10.0, 11.0), (20.0, 21.0), (30.0, 31.0), (40.0, 41.0)]]
    assert filter_plm_group_members(groups, 15.0, 41.0) == [(20.0, 21.0), (30.0, 31.0), (40.0, 41.0)]
    assert filter_plm_group_members(groups, 100.0, 200.0) == []


def test_windowed_hours_uses_requested_span_not_padded_span():
    assert windowed_hours(0.0, 3600.0) == pytest.approx(1.0)
    assert windowed_hours(1000.0, 1000.0 + 1800.0) == pytest.approx(0.5)
