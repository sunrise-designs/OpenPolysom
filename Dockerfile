FROM python:3.11-bookworm

# Install Node.js 20.x, gcc, git, and build tools
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        curl ca-certificates gnupg git gcc g++ pkg-config && \
    curl -fsSL https://deb.nodesource.com/setup_20.x | bash - && \
    apt-get install -y --no-install-recommends nodejs && \
    rm -rf /var/lib/apt/lists/*

# Rust toolchain (src_rust/polysom_rtdi). Installed globally, independent of
# repo source, so it caches well as its own layer.
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:$PATH
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --profile minimal --default-toolchain stable && \
    rustc --version && cargo --version

WORKDIR /app

# ── Python dependencies (cache layer) ────────────────────────────────────────
COPY src_python/requirements.txt src_python/requirements.txt
RUN pip install --no-cache-dir -r src_python/requirements.txt

# ── npm dependencies (cache layer) ───────────────────────────────────────────
# npm ci installs devDependencies too (eslint, typescript-eslint, vitest,
# @playwright/test, husky, lint-staged) alongside the runtime deps.
COPY src_web/package.json src_web/package-lock.json src_web/
RUN cd src_web && npm ci

# Playwright browser binary + its OS-level deps (chromium only, to keep the
# image lean; add firefox/webkit here too if a test suite needs them).
RUN cd src_web && npx playwright install --with-deps chromium

# ── Full source + data files ──────────────────────────────────────────────────
COPY . .

# Build web frontend (creates src_web/dist/chart.js, required by deploy.py)
RUN cd src_web && npm run build

# No display in container; force unbuffered stdout so output streams in real time
ENV MPLBACKEND=Agg \
    PYTHONUNBUFFERED=1

CMD ["/bin/bash", "/app/docker_run.sh"]
