import argparse
import io
import json
import sys
import zipfile
from pathlib import Path

import requests

from plotting import PLOTLY_HTML_OUT

_DIR            = Path(__file__).parent
_NETLIFY_CONFIG = _DIR / 'netlify.json'
_NETLIFY_API    = 'https://api.netlify.com/api/v1/sites/{site_id}/deploys'


def deploy_to_netlify(html_path=PLOTLY_HTML_OUT):
    config  = json.loads(_NETLIFY_CONFIG.read_text())
    site_id = config['site_id']

    token = config.get('token')
    if not token:
        print('Error: add a "token" key to netlify.json.', file=sys.stderr)
        sys.exit(1)

    # Build zip in memory — the file is always named index.html inside the archive.
    # _headers tells Netlify to serve it as text/html; without it zip deploys may
    # default to text/plain and the browser shows source instead of rendering.
    html_bytes = Path(html_path).read_bytes()
    headers_file = '/index.html\n  Content-Type: text/html; charset=utf-8\n'
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as zf:
        zf.writestr('index.html', html_bytes)
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
        timeout=60,
    )
    resp.raise_for_status()

    deploy = resp.json()
    live_url = deploy.get('ssl_url') or deploy.get('url') or '(url pending)'
    print(f"Deployed to: {live_url}")
    return deploy


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Deploy a Plotly HTML chart to Netlify.')
    parser.add_argument('--file', default=PLOTLY_HTML_OUT,
                        help=f'HTML file to deploy (default: {PLOTLY_HTML_OUT})')
    args = parser.parse_args()
    deploy_to_netlify(args.file)
