#!/usr/bin/env bash
#
# Build the Quartz read-site for the ProtoSom wiki into ./public.
#
# Quartz v5 is not an npm package; it is used by cloning its repo. This script
# clones Quartz (pinned to a commit) into ./.quartz (gitignored, never committed),
# installs the community plugins pinned by Quartz's quartz.lock.json, overlays our
# committed web/quartz.config.yaml, mirrors the wiki/ folder into the clone's
# content/ folder (renaming INDEX.md to index.md so it becomes the site root), and
# builds the static site into ./public.
#
# Unlike a dedicated wiki repo, ProtoSom is a CODE repo: the wiki lives in the
# wiki/ subfolder, so ONLY wiki/ is mirrored into content/ (the src/, src_python/,
# src_web/, doc/, plans/ trees are never published).
#
# This script writes ONLY to .quartz/ and public/ (both gitignored); it never
# mutates tracked source files. Local dev and CI run this identical path. No
# secrets are involved in the build; deployment is handled by deploy-pages.yml.
#
# Usage:
#   bash web/build.sh                   # clone/refresh Quartz, build to ./public
#   QUARTZ_REF=<sha> bash web/build.sh  # build against a different Quartz commit
#   python3 -m http.server -d public 8080   # preview the built site locally
#
# Requires: git, rsync, Node >= 22 and npm >= 10.9.2, and network access at build
# time to clone the pinned community plugins from GitHub.

set -euo pipefail

# Pinned Quartz commit: v5 branch tip (Quartz 5.0.0). The clone's quartz.lock.json
# pins each community plugin to a commit, so `quartz plugin install` is
# reproducible at this ref. Override with the QUARTZ_REF env var.
QUARTZ_REF="${QUARTZ_REF:-c5bbc92b74f67441fbfc0b1f5ae7ad15ba8e61af}"
QUARTZ_URL="https://github.com/jackyzha0/quartz.git"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUARTZ_DIR="$ROOT/.quartz"
REF_MARKER="$QUARTZ_DIR/.protosom-build-ref"

# 1. Clone Quartz at the pinned commit, or refresh an existing clone to it.
if [ ! -d "$QUARTZ_DIR/.git" ]; then
  echo "==> Cloning Quartz @ ${QUARTZ_REF}"
  git init -q "$QUARTZ_DIR"
  git -C "$QUARTZ_DIR" remote add origin "$QUARTZ_URL"
fi
git -C "$QUARTZ_DIR" fetch -q --depth 1 origin "$QUARTZ_REF"
git -C "$QUARTZ_DIR" checkout -q -f FETCH_HEAD

# 2. Install Quartz dependencies and the pinned community plugins (cached in CI;
#    skipped on a fast local rebuild when the clone is already at this ref).
if [ "$(cat "$REF_MARKER" 2>/dev/null || true)" != "$QUARTZ_REF" ] \
   || [ ! -d "$QUARTZ_DIR/node_modules" ] || [ ! -d "$QUARTZ_DIR/.quartz/plugins" ]; then
  echo "==> Installing Quartz dependencies"
  ( cd "$QUARTZ_DIR" && npm ci --no-audit --no-fund )
  echo "==> Installing pinned community plugins (from quartz.lock.json)"
  ( cd "$QUARTZ_DIR" && npx quartz plugin install )
  echo "$QUARTZ_REF" > "$REF_MARKER"
fi

# 3. Overlay our Quartz configuration onto the clone.
echo "==> Applying quartz.config.yaml"
cp "$ROOT/web/quartz.config.yaml" "$QUARTZ_DIR/quartz.config.yaml"

# 4. Mirror ONLY the wiki/ folder into the clone's content/. rsync --delete makes
#    the destination match the source exactly (cleans stale files without rm -rf)
#    while preserving the directory tree, so the relative .md links resolve.
echo "==> Mirroring wiki/ content"
mkdir -p "$QUARTZ_DIR/content"
rsync -a --delete \
  --exclude '.obsidian/' \
  --exclude '.DS_Store' \
  "$ROOT/wiki/" "$QUARTZ_DIR/content/"

# 4b. The wiki cites repo source files with repo-root-relative links (../../path).
#     Those files are NOT published, so they would 404 on the site. Rewrite them to
#     GitHub blob URLs in the build copy ONLY — the committed wiki/ keeps clean
#     relative paths for in-repo and agent reading. Wiki-internal links use a single
#     ../ (never ../../), so they are untouched; only repo-escaping links are remapped.
echo "==> Rewriting repo-file links to GitHub URLs (build copy only)"
GH_BLOB="https://github.com/sunrise-designs/OpenPolysom/blob/main"
find "$QUARTZ_DIR/content" -name '*.md' -print0 \
  | xargs -0 perl -pi -e "s{\]\(\Q../../\E}{](${GH_BLOB}/}g"

# 4c. The wiki's root file is INDEX.md, but the site root must be a lowercase
#     index (Quartz slugs are case-sensitive, so INDEX != index). Rewrite link
#     TARGETS that point at INDEX.md to index.md in the build copy only; prose
#     mentions of "INDEX" are left untouched. The committed wiki keeps INDEX.md
#     so its real filename resolves for in-repo and agent reading.
echo "==> Rewriting INDEX.md link targets to index.md (build copy only)"
find "$QUARTZ_DIR/content" -name '*.md' -print0 \
  | xargs -0 perl -pi -e 's{\]\(([^)]*?)INDEX\.md}{]($1index.md}g'

# 5. The site root must be a lowercase index.md; the wiki's entry point is INDEX.md.
#    Rename it in the build copy only. The committed repo keeps INDEX.md.
if [ -f "$QUARTZ_DIR/content/INDEX.md" ]; then
  mv "$QUARTZ_DIR/content/INDEX.md" "$QUARTZ_DIR/content/index.md"
fi

# 6. Build the static site into ./public (gitignored).
echo "==> Building site"
( cd "$QUARTZ_DIR" && npx quartz build -o "$ROOT/public" )

echo
echo "==> Built to $ROOT/public"
echo "    Preview locally:  python3 -m http.server -d \"$ROOT/public\" 8080"
echo "    (then open http://localhost:8080)"
