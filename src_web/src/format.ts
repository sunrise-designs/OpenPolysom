/**
 * Pure formatting helpers (functional core). No DOM, no I/O — unit tested.
 */

/** Seconds → "6h 02m" / "47m 12s" / "8s". */
export function formatDuration(totalSeconds: number): string {
  const s = Math.max(0, Math.floor(totalSeconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${String(h)}h ${String(m).padStart(2, '0')}m`;
  if (m > 0) return `${String(m)}m ${String(sec).padStart(2, '0')}s`;
  return `${String(sec)}s`;
}

/** Seconds-from-midnight-style clock for a millisecond instant: "01:42:10". */
export function formatClock(ms: number): string {
  return new Date(ms).toISOString().substring(11, 19);
}

/** Elapsed seconds → "HH:MM:SS" for axis labels. */
export function formatElapsed(seconds: number): string {
  const s = Math.max(0, Math.floor(seconds));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return [h, m, sec].map((n) => String(n).padStart(2, '0')).join(':');
}

/** ISO datetime → "14 Jun 2026 · 23:08" (locale-independent, UTC). */
export function formatRecordedAt(iso: string): string {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return iso;
  const months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'] as const;
  const day = d.getUTCDate();
  const mon = months[d.getUTCMonth()] ?? '';
  const year = d.getUTCFullYear();
  const hh = String(d.getUTCHours()).padStart(2, '0');
  const mm = String(d.getUTCMinutes()).padStart(2, '0');
  return `${String(day)} ${mon} ${String(year)} · ${hh}:${mm}`;
}

/** Short git SHA (first 7 chars) with a dirty marker. */
export function shortSha(sha: string, dirty: boolean): string {
  const short = sha.length > 7 ? sha.substring(0, 7) : sha;
  return dirty ? `${short}-dirty` : short;
}
