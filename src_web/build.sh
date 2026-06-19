#!/usr/bin/env sh
# Rebuild dist/chart.js from TypeScript source.
# Run from the src_web/ directory.
npx esbuild src/main.ts --bundle --outfile=dist/chart.js --platform=browser --target=es2020 --minify
echo "Built dist/chart.js"
