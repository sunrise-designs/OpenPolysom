import type { BrushRange } from './chart';
import type { MetricRequestItem, MetricsRequest, MetricsResponse } from './types';

/**
 * The viewer's first outbound network call — everywhere else (`zarr_loader.ts`)
 * only reads the Zarr boundary. Non-2xx and network failures both resolve to
 * `{ ok: false }` rather than throwing, so a metrics-service outage degrades
 * the result card to "unavailable" instead of breaking the chart.
 */
export type MetricsClientResult =
  | { readonly ok: true; readonly data: MetricsResponse }
  | { readonly ok: false; readonly reason: string };

export async function requestWindowedMetrics(
  baseUrl: string,
  recordingId: string,
  range: BrushRange,
  metrics: readonly MetricRequestItem[],
): Promise<MetricsClientResult> {
  const body: MetricsRequest = { window: { start_s: range[0], end_s: range[1] }, metrics };

  try {
    const response = await fetch(`${baseUrl}/v1/recordings/${encodeURIComponent(recordingId)}/metrics`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    if (!response.ok) return { ok: false, reason: `metrics service returned ${String(response.status)}` };
    const data = (await response.json()) as MetricsResponse;
    return { ok: true, data };
  } catch {
    return { ok: false, reason: 'metrics service unavailable' };
  }
}
