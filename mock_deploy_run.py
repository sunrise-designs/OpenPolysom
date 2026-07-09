#!/usr/bin/env python3
"""Run deploy.py with requests.get/post/put mocked — builds the file manifest
and would-be uploads but never touches the network."""
import os
import sys
from unittest.mock import MagicMock, patch

# Repo root must be CWD so deploy.py resolves zarr/meta paths via Path.cwd()
os.chdir(os.path.dirname(os.path.abspath(__file__)))

MOCK_SITE_URL = "https://mock-deploy.netlify.app"


def _mock_get(url, headers=None, timeout=None, **_kwargs):
    """Matches by URL shape, not the real site_id from netlify.json, so this
    works whatever site_id/token happen to be in the local (gitignored) config."""
    resp = MagicMock()
    resp.raise_for_status.return_value = None
    if url.endswith('/studies.json'):
        # Simulate a fresh site: no studies deployed yet.
        resp.ok = False
        resp.status_code = 404
        return resp
    if '/deploys/' in url and url.endswith('/files'):
        # A previous deploy's file listing — none, since published_deploy is None below.
        resp.ok = True
        resp.status_code = 200
        resp.json.return_value = []
        return resp
    # GET /sites/{site_id}
    resp.ok = True
    resp.status_code = 200
    resp.json.return_value = {
        "id": "mock-site-id", "ssl_url": MOCK_SITE_URL, "url": MOCK_SITE_URL,
        "published_deploy": None,  # no previous deploy -> nothing to fold into the new manifest
    }
    return resp


def _mock_post(url, headers=None, json=None, timeout=None, **_kwargs):
    resp = MagicMock()
    resp.status_code = 200
    resp.raise_for_status.return_value = None
    manifest = (json or {}).get('files', {})
    resp.json.return_value = {
        "id": "mock-deploy-abc123", "state": "ready",
        "ssl_url": MOCK_SITE_URL, "url": MOCK_SITE_URL,
        "required": list(manifest.values()),  # pretend Netlify has none of it yet
    }
    return resp


def _mock_put(url, headers=None, data=None, timeout=None, **_kwargs):
    resp = MagicMock()
    resp.status_code = 200
    resp.raise_for_status.return_value = None
    return resp


sys.argv = ["deploy.py", "--stem", "biometric_filtered"]

print("[MOCK] requests.get/post/put are intercepted — no network upload will occur\n")

with patch("requests.get", side_effect=_mock_get), \
     patch("requests.post", side_effect=_mock_post) as mock_post, \
     patch("requests.put", side_effect=_mock_put) as mock_put:
    import runpy
    try:
        runpy.run_path("src_python/deploy.py", run_name="__main__")
    except SystemExit as exc:
        if exc.code not in (0, None):
            print(f"[ERROR] deploy.py exited with code {exc.code}", file=sys.stderr)
            sys.exit(exc.code)

    if mock_post.called:
        url_called = (mock_post.call_args[0] or [None])[0] or "?"
        print(f"\n[MOCK] Intercepted deploy POST to: {url_called}")
    print(f"[MOCK] Intercepted {mock_put.call_count} file upload PUT(s)")

print("[MOCK] Manifest built and upload mocked successfully")
