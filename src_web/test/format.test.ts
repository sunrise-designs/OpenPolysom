import { describe, it, expect } from 'vitest';
import { formatDuration, formatElapsed, formatRecordedAt, shortSha } from '../src/format';

describe('formatDuration', () => {
  it('formats hours and minutes', () => {
    expect(formatDuration(6 * 3600 + 2 * 60)).toBe('6h 02m');
  });
  it('formats minutes and seconds under an hour', () => {
    expect(formatDuration(47 * 60 + 12)).toBe('47m 12s');
  });
  it('formats seconds only', () => {
    expect(formatDuration(8)).toBe('8s');
  });
  it('clamps negatives to zero', () => {
    expect(formatDuration(-5)).toBe('0s');
  });
});

describe('formatElapsed', () => {
  it('zero-pads h:m:s', () => {
    expect(formatElapsed(3661)).toBe('01:01:01');
  });
});

describe('formatRecordedAt', () => {
  it('renders a UTC human date', () => {
    expect(formatRecordedAt('2026-06-14T23:08:00+00:00')).toBe('14 Jun 2026 · 23:08');
  });
  it('passes through an unparseable value', () => {
    expect(formatRecordedAt('not-a-date')).toBe('not-a-date');
  });
});

describe('shortSha', () => {
  it('shortens a full SHA', () => {
    expect(shortSha('81955d3abc1234567890', false)).toBe('81955d3');
  });
  it('marks dirty builds', () => {
    expect(shortSha('81955d3abc', true)).toBe('81955d3-dirty');
  });
});
