// Deployment-specific config, un-bundled so it can be edited per-environment
// without a rebuild (same idea as metrics-config.js/styles.css/sw.js — a static
// asset served beside index.html, not part of the esbuild bundle).
//
// Points the viewer at a device's real-time sample stream (see
// ESP32-C6-heart-idf/components/rt_stream). Setting it makes the viewer open in
// RT mode on every load, so it is left unset by default: the usual way in is
// the per-visit `?rt=` query parameter, e.g.
//
//   index.html?rt=192.168.4.1        (the device's own SoftAP — expands to ws://192.168.4.1/rt)
//   index.html?rt=ws://host:8080/rt
//
// Set this instead only for a fixed installation that should always come up
// live. Unset (or deleted) means the viewer behaves exactly as before: `?meta=`
// opens a stored recording, no parameter shows the landing page.
window.PROTOSOM_RT_URL = '';
