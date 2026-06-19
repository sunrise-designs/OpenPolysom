import * as echarts from 'echarts';
import { loadMeta, loadZarr } from './zarr_loader';
import { buildChartOption, buildTitle } from './chart';

async function main() {
  const params = new URLSearchParams(window.location.search);
  const metaParam = params.get('meta');
  if (!metaParam) {
    document.getElementById('title')!.textContent = 'No ?meta= parameter supplied.';
    return;
  }

  // Resolve zarr base URL relative to meta JSON location
  const metaUrl = new URL(metaParam, window.location.href).href;
  const baseDir = metaUrl.substring(0, metaUrl.lastIndexOf('/') + 1);

  try {
    const meta = await loadMeta(metaUrl);
    document.getElementById('title')!.innerHTML = buildTitle(meta);

    const zarrUrl = new URL(meta.zarr_path, baseDir).href;
    const zarrData = await loadZarr(zarrUrl);

    const chartEl = document.getElementById('chart')!;
    const chart = echarts.init(chartEl, 'dark', { renderer: 'canvas' });
    chart.setOption(buildChartOption(zarrData, meta));
    window.addEventListener('resize', () => chart.resize());
  } catch (err) {
    document.getElementById('title')!.textContent = `Error: ${err}`;
    console.error(err);
  }
}

main();
