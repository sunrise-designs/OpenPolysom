// Deployment-specific config, un-bundled so it can be edited per-environment
// without a rebuild (same idea as styles.css/sw.js/manifest.webmanifest — a
// static asset served beside index.html, not part of the esbuild bundle).
// Points the viewer at the windowed clinical-metrics service (see
// src_python/metrics_service.py + src_python/serve_metrics.py). Leave unset
// (or delete this line) to disable the windowed-metrics card entirely — the
// chart works fine without it.
window.PROTOSOM_METRICS_URL = 'http://localhost:8800';
