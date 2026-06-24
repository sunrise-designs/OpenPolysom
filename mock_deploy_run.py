#!/usr/bin/env python3
"""Run deploy.py with requests.post mocked — builds the ZIP but never uploads."""
import os
import sys
from unittest.mock import MagicMock, patch

# Repo root must be CWD so deploy.py resolves zarr/meta paths via Path.cwd()
os.chdir(os.path.dirname(os.path.abspath(__file__)))

mock_response = MagicMock()
mock_response.status_code = 200
mock_response.json.return_value = {
    "id":      "mock-deploy-abc123",
    "state":   "ready",
    "ssl_url": "https://mock-deploy.netlify.app",
    "url":     "https://mock-deploy.netlify.app",
}
mock_response.raise_for_status.return_value = None

sys.argv = ["deploy.py", "--stem", "biometric_filtered"]

print("[MOCK] requests.post is intercepted — no network upload will occur\n")

with patch("requests.post", return_value=mock_response) as mock_post:
    import runpy
    try:
        runpy.run_path("src_python/deploy.py", run_name="__main__")
    except SystemExit as exc:
        if exc.code not in (0, None):
            print(f"[ERROR] deploy.py exited with code {exc.code}", file=sys.stderr)
            sys.exit(exc.code)

    if mock_post.called:
        url_called = (mock_post.call_args[0] or [None])[0] or "?"
        print(f"\n[MOCK] Intercepted POST to: {url_called}")

print("[MOCK] ZIP built and upload mocked successfully")
