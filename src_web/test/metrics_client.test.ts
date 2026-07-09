import { describe, it, expect, vi, afterEach } from 'vitest';
import { requestWindowedMetrics } from '../src/metrics_client';
import type { MetricRequestItem, MetricsResponse } from '../src/types';

const items: readonly MetricRequestItem[] = [{ metric: 'plmi', channel: 'accel_mag', params: { threshold: 8.0 } }];

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('requestWindowedMetrics', () => {
  it('POSTs the window/metrics request body in the documented shape', async () => {
    const fetchMock = vi.fn(async () =>
      new Response(JSON.stringify({
        recording_id: 'rec-1',
        requested_window: { start_s: 100, end_s: 140 },
        results: [],
        provenance: { git: { sha: 'abc', dirty: false, branch: 'main' }, service: 'metrics_service', computed_at: 'now' },
      } satisfies MetricsResponse), { status: 200, headers: { 'Content-Type': 'application/json' } }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await requestWindowedMetrics('http://localhost:8800', 'rec-1', [100, 140], items);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [url, init] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('http://localhost:8800/v1/recordings/rec-1/metrics');
    expect(init.method).toBe('POST');
    expect(JSON.parse(init.body as string)).toEqual({
      window: { start_s: 100, end_s: 140 },
      metrics: items,
    });
    expect(result.ok).toBe(true);
    if (result.ok) expect(result.data.recording_id).toBe('rec-1');
  });

  it('URL-encodes the recording id', async () => {
    const fetchMock = vi.fn(async () => new Response('{}', { status: 200 }));
    vi.stubGlobal('fetch', fetchMock);

    await requestWindowedMetrics('http://localhost:8800', 'a b/c', [0, 20], items);

    const [url] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('http://localhost:8800/v1/recordings/a%20b%2Fc/metrics');
  });

  it('resolves to a typed error on a non-2xx response, without throwing', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => new Response('not found', { status: 404 })));

    const result = await requestWindowedMetrics('http://localhost:8800', 'missing', [0, 20], items);

    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toContain('404');
  });

  it('resolves to a typed error on a network failure, without throwing', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => { throw new TypeError('network down'); }));

    const result = await requestWindowedMetrics('http://localhost:8800', 'rec-1', [0, 20], items);

    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.reason).toBe('metrics service unavailable');
  });

  it('resolves to a typed error when the response body is not valid JSON', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => new Response('<html>not json</html>', { status: 200 })));

    const result = await requestWindowedMetrics('http://localhost:8800', 'rec-1', [0, 20], items);

    expect(result.ok).toBe(false);
  });
});
