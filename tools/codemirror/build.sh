#!/bin/bash
# Builds components/web/ui/cm.js (CodeMirror 6 + Lua mode, one minified file) from the
# pinned versions in package.json. Run only when changing the editor or updating
# CodeMirror; the firmware build just embeds the committed cm.js (no Node needed).
# Everything stays in the repo: npm's cache and node_modules live under .esp/.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$ROOT/.esp/codemirror"
export npm_config_cache="$ROOT/.esp/npm-cache" npm_config_update_notifier=false npm_config_fund=false
mkdir -p "$WORK"
cp "$HERE/package.json" "$HERE/entry.js" "$WORK/"
[[ -f "$HERE/package-lock.json" ]] && cp "$HERE/package-lock.json" "$WORK/"
cd "$WORK"
if [[ -f package-lock.json ]]; then npm ci --no-audit; else npm install --no-audit; fi
cp package-lock.json "$HERE/"
npx esbuild entry.js --bundle --minify --format=iife --target=es2020 --legal-comments=eof \
  --banner:js="/* CodeMirror 6 (codemirror.net) + Lua mode, MIT, (C) Marijn Haverbeke and others; see tools/codemirror/LICENSE. Built by tools/codemirror/build.sh */" \
  --outfile="$ROOT/components/web/ui/cm.js"
echo "cm.js: $(wc -c < "$ROOT/components/web/ui/cm.js") bytes, $(gzip -9 -n -c "$ROOT/components/web/ui/cm.js" | wc -c) gzipped"
