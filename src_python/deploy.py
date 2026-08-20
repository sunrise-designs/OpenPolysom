import argparse
import hashlib
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import requests

from export_zarr import DEFAULT_OUT_DIR, find_meta

_DIR            = Path(__file__).parent
_SRC_WEB        = _DIR.parent / 'src_web'
_NETLIFY_CONFIG = _DIR / 'netlify.json'
_API            = 'https://api.netlify.com/api/v1'

# Static PWA assets index.html/manifest.webmanifest need, beyond dist/chart.js —
# shared across every study. Their bytes rarely change, so Netlify's digest
# dedup (see deploy_to_netlify) means they're uploaded once and then reused by
# every later deploy, however many studies get added.
#
# IMPORTANT: any new un-bundled static file src_web/index.html references
# (a <script src=...>/<link href=...> pointing outside dist/chart.js) must be
# added here too, or it silently 404s live — deploy.py has no way to discover
# it automatically. metrics-config.js was missed this way when it was added;
# don't repeat that.
_STATIC_ASSETS = ('styles.css', 'sw.js', 'manifest.webmanifest', 'metrics-config.js',
                   'rt-config.js', 'icon.svg', 'icon-192.png', 'icon-512.png')

_HEADERS_FILE = (
    '/index.html\n  Content-Type: text/html; charset=utf-8\n'
    '/dist/chart.js\n  Content-Type: application/javascript\n'
    '/sw.js\n  Content-Type: application/javascript\n'
    '/styles.css\n  Content-Type: text/css\n'
    '/manifest.webmanifest\n  Content-Type: application/manifest+json\n'
    '/metrics-config.js\n  Content-Type: application/javascript\n'
    '/rt-config.js\n  Content-Type: application/javascript\n'
    '/studies.json\n  Content-Type: application/json\n'
    '/studies/*/meta.json\n  Content-Type: application/json\n'
    '/studies/*/events.json\n  Content-Type: application/json\n'
)


def _sha1(data):
    return hashlib.sha1(data).hexdigest()


def _shared_assets():
    """Site-wide files common to every study: the viewer shell + chart bundle."""
    files = {
        '/index.html':    (_SRC_WEB / 'index.html').read_bytes(),
        '/dist/chart.js': (_SRC_WEB / 'dist' / 'chart.js').read_bytes(),
        '/_headers':      _HEADERS_FILE.encode('utf-8'),
    }
    for name in _STATIC_ASSETS:
        files[f'/{name}'] = (_SRC_WEB / name).read_bytes()
    return files


def _study_files(recording_id, zarr_path, meta_path, events_path):
    """This study's files, namespaced under studies/<recording_id>/ so many
    studies coexist in one deploy instead of each replacing the last."""
    prefix = f'/studies/{recording_id}'
    files = {
        f'{prefix}/meta.json':   meta_path.read_bytes(),
        f'{prefix}/events.json': events_path.read_bytes(),
    }
    for file_path in sorted(Path(zarr_path).rglob('*')):
        if file_path.is_file():
            arc = file_path.relative_to(zarr_path).as_posix()
            files[f'{prefix}/{zarr_path.name}/{arc}'] = file_path.read_bytes()
    return files


def _study_summary(meta):
    stats      = meta.get('stats', {})
    recording  = meta.get('recording', {})
    hrv_raw    = stats.get('hrv_rmssd_overall')
    return {
        'recording_id':      meta['recording_id'],
        'meta_path':         f"studies/{meta['recording_id']}/meta.json",
        'subject_id':        meta.get('subject', {}).get('subject_id', '?'),
        'start_iso':         recording.get('start_iso', ''),
        'duration_s':        recording.get('duration_s', 0),
        'plmi':              stats.get('plmi', 0.0),
        'total_lms':         stats.get('total_lms', 0),
        'hrv_rmssd_overall': hrv_raw,
        'deployed_at':       datetime.now(timezone.utc).isoformat(),
    }


def _netlify_get(path, token):
    resp = requests.get(f'{_API}{path}', headers={'Authorization': f'Bearer {token}'}, timeout=30)
    resp.raise_for_status()
    return resp.json()


def _previous_files(site, token):
    """Every path -> sha1 already published on the site (from its last live
    deploy). Folded into the new manifest unchanged (no bytes re-sent) so
    Netlify's digest dedup skips re-uploading anything that hasn't changed —
    this is what stops a new study's deploy from re-shipping every prior one."""
    deploy = site.get('published_deploy')
    if not deploy:
        return {}
    entries = _netlify_get(f"/deploys/{deploy['id']}/files", token)
    return {e['path']: e['sha'] for e in entries}


def _previous_studies(site_url):
    try:
        resp = requests.get(f'{site_url}/studies.json', timeout=30)
        if resp.ok:
            return resp.json()
    except requests.RequestException:
        pass
    return []


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

    meta = json.loads(meta_path.read_text())
    recording_id = meta['recording_id']

    site         = _netlify_get(f'/sites/{site_id}', token)
    site_url     = site.get('ssl_url') or site.get('url')
    prev_files   = _previous_files(site, token)
    prev_studies = [s for s in _previous_studies(site_url) if s.get('recording_id') != recording_id]
    studies      = sorted([*prev_studies, _study_summary(meta)], key=lambda s: s.get('start_iso', ''), reverse=True)

    local_files = {
        **_shared_assets(),
        **_study_files(recording_id, zarr_path, meta_path, events_path),
        '/studies.json': json.dumps(studies, indent=2).encode('utf-8'),
    }

    # The full manifest = every file the *site* should contain (deploys are
    # atomic snapshots), but only local_files carries bytes we can upload —
    # anything from prev_files that we didn't touch, Netlify already has.
    manifest = {**prev_files, **{path: _sha1(data) for path, data in local_files.items()}}

    resp = requests.post(
        f'{_API}/sites/{site_id}/deploys',
        headers={'Content-Type': 'application/json', 'Authorization': f'Bearer {token}'},
        json={'files': manifest},
        timeout=60,
    )
    resp.raise_for_status()
    deploy = resp.json()

    digest_to_path = {digest: path for path, digest in manifest.items()}
    required = deploy.get('required') or []
    uploaded = 0
    for digest in required:
        path = digest_to_path.get(digest)
        data = local_files.get(path) if path is not None else None
        if data is None:
            print(f'Warning: Netlify requested an unrecognised digest {digest}', file=sys.stderr)
            continue
        put = requests.put(
            f"{_API}/deploys/{deploy['id']}/files{path}",
            headers={'Content-Type': 'application/octet-stream', 'Authorization': f'Bearer {token}'},
            data=data,
            timeout=120,
        )
        put.raise_for_status()
        uploaded += 1

    reused = len(manifest) - len(required)
    print(f"Uploaded {uploaded} new/changed file(s); reused {reused} already on Netlify.")
    print(f"Deployed: {site_url}/index.html?meta=studies/{recording_id}/meta.json")
    print(f"Landing page (all studies): {site_url}/")
    return deploy


def _resolve_paths(stem=None, zarr=None, meta=None):
    """Derive zarr/meta paths from a stem, explicit args, or auto-discovery.

    Discovery goes through `find_meta`, so it looks in `data_scratchpad/` (where the
    pipeline writes) before the cwd — the same rule serve.py follows.
    """
    if zarr and meta:
        return Path(zarr), Path(meta)
    meta_path = find_meta(stem=stem)
    if meta_path is None or not meta_path.exists():
        print(f'Error: no *_meta.json found in {DEFAULT_OUT_DIR}/ or cwd. '
              'Use --stem or --zarr/--meta.', file=sys.stderr)
        sys.exit(1)
    if stem:
        return meta_path.parent / f'{stem}.zarr', meta_path
    meta_data = json.loads(meta_path.read_text())
    zarr_name = meta_data.get('layers', {}).get('working', {}).get('path')
    if not zarr_name:
        print(f'Error: no layers.working.path key in {meta_path}', file=sys.stderr)
        sys.exit(1)
    return meta_path.parent / zarr_name, meta_path


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='Deploy a study to the shared Netlify site (adds it to the landing page; does not replace other studies).',
        epilog='Run from the repo root; outputs resolve from data_scratchpad/ first. Examples:\n'
               '  python src_python/deploy.py                              # auto-detect newest *_meta.json\n'
               '  python src_python/deploy.py --stem biometric_filtered    # explicit stem\n'
               '  python src_python/deploy.py --zarr data_scratchpad/biometric_filtered.zarr --meta data_scratchpad/biometric_filtered_meta.json',
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
