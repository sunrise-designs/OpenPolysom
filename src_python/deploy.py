import argparse
import io
import json
import sys
import zipfile
from pathlib import Path

import requests

_DIR            = Path(__file__).parent
_SRC_WEB        = _DIR.parent / 'src_web'
_NETLIFY_CONFIG = _DIR / 'netlify.json'
_NETLIFY_API    = 'https://api.netlify.com/api/v1/sites/{site_id}/deploys'

# Static PWA assets index.html/manifest.webmanifest need, beyond dist/chart.js —
# Netlify only serves what's in the zip, unlike serve.py's on-the-fly lookup.
_STATIC_ASSETS = ('styles.css', 'sw.js', 'manifest.webmanifest', 'icon.svg', 'icon-192.png', 'icon-512.png')


def _add_dir_to_zip(zf, dir_path, zip_prefix):
    for file_path in sorted(Path(dir_path).rglob('*')):
        if file_path.is_file():
            arc_name = zip_prefix + '/' + file_path.relative_to(dir_path).as_posix()
            zf.write(file_path, arc_name)


def deploy_to_netlify(zarr_path, meta_path):
    config  = json.loads(_NETLIFY_CONFIG.read_text())
    site_id = config['site_id']

    token = config.get('token')
    if not token:
        print('Error: add a "token" key to netlify.json.', file=sys.stderr)
        sys.exit(1)

    zarr_path   = Path(zarr_path)
    meta_path   = Path(meta_path)
    events_path = meta_path.parent / 'events.json'

    index_html   = _SRC_WEB / 'index.html'
    chart_js     = _SRC_WEB / 'dist' / 'chart.js'
    static_paths = [_SRC_WEB / name for name in _STATIC_ASSETS]

    for p in (zarr_path, meta_path, events_path, index_html, chart_js, *static_paths):
        if not p.exists():
            print(f'Error: not found: {p}', file=sys.stderr)
            sys.exit(1)

    headers_file = (
        '/index.html\n  Content-Type: text/html; charset=utf-8\n'
        '/dist/chart.js\n  Content-Type: application/javascript\n'
        '/sw.js\n  Content-Type: application/javascript\n'
        '/styles.css\n  Content-Type: text/css\n'
        '/manifest.webmanifest\n  Content-Type: application/manifest+json\n'
        f'/{meta_path.name}\n  Content-Type: application/json\n'
        '/events.json\n  Content-Type: application/json\n'
    )

    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('index.html', index_html.read_bytes())
        zf.writestr('dist/chart.js', chart_js.read_bytes())
        for static_path in static_paths:
            zf.writestr(static_path.name, static_path.read_bytes())
        zf.writestr(meta_path.name, meta_path.read_bytes())
        zf.writestr('events.json', events_path.read_bytes())
        _add_dir_to_zip(zf, zarr_path, zarr_path.name)
        zf.writestr('_headers', headers_file)
    buf.seek(0)

    url  = _NETLIFY_API.format(site_id=site_id)
    resp = requests.post(
        url,
        headers={
            'Content-Type':  'application/zip',
            'Authorization': f'Bearer {token}',
        },
        data=buf.read(),
        timeout=120,
    )
    resp.raise_for_status()

    deploy   = resp.json()
    live_url = deploy.get('ssl_url') or deploy.get('url') or '(url pending)'
    print(f"Deployed to: {live_url}?meta={meta_path.name}")
    return deploy


def _resolve_paths(stem=None, zarr=None, meta=None):
    """Derive zarr/meta paths from a stem, explicit args, or auto-discovery in cwd."""
    cwd = Path.cwd()
    if stem:
        return cwd / f'{stem}.zarr', cwd / f'{stem}_meta.json'
    if zarr and meta:
        return Path(zarr), Path(meta)
    # Auto-discover: pick the most recently modified *_meta.json in cwd.
    candidates = sorted(cwd.glob('*_meta.json'), key=lambda p: p.stat().st_mtime, reverse=True)
    if not candidates:
        print('Error: no *_meta.json found in cwd. Use --stem or --zarr/--meta.', file=sys.stderr)
        sys.exit(1)
    meta_path = candidates[0]
    meta_data = json.loads(meta_path.read_text())
    zarr_name = meta_data.get('layers', {}).get('working', {}).get('path')
    if not zarr_name:
        print(f'Error: no layers.working.path key in {meta_path}', file=sys.stderr)
        sys.exit(1)
    return cwd / zarr_name, meta_path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Deploy the biometric viewer to Netlify.',
        epilog='Run from the repo root. Examples:\n'
               '  python src_python/deploy.py                              # auto-detect newest *_meta.json\n'
               '  python src_python/deploy.py --stem biometric_filtered    # explicit stem\n'
               '  python src_python/deploy.py --zarr biometric_filtered.zarr --meta biometric_filtered_meta.json',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--stem', help='Base name shared by the .zarr dir and _meta.json (e.g. biometric_filtered)')
    parser.add_argument('--zarr', help='Path to the .zarr store directory (used with --meta)')
    parser.add_argument('--meta', help='Path to the _meta.json sidecar file (used with --zarr)')
    args = parser.parse_args()

    if bool(args.zarr) != bool(args.meta):
        parser.error('--zarr and --meta must be used together')

    zarr_p, meta_p = _resolve_paths(stem=args.stem, zarr=args.zarr, meta=args.meta)
    deploy_to_netlify(zarr_p, meta_p)
