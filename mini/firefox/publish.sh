#!/usr/bin/env bash
# ===========================================================================
#  BoltOS Mini  -  mini/firefox/publish.sh
#
#  Puts the Firefox machine's files (build/mini/firefox, from make-image.sh)
#  on boltos.dev, where a Mini's Browser fetches them (mini/fxget.c): the R2
#  bucket the packages live in, under firefox/<version>/, then the list that
#  says which version is current, last, so no Mini ever sees a list whose
#  files aren't all there yet.
#
#      bash mini/firefox/publish.sh [--dry-run]
#
#  The image holds no Microsoft fonts (their licence doesn't allow passing
#  them on); each Mini fetches those itself on its first start.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/build/mini/firefox"
BUCKET=boltos-packages
for f in Image root.sqfs version; do [ -f "$OUT/$f" ] || { echo "ERROR: $OUT/$f missing -- run make-image.sh"; exit 1; }; done
FX=$(sed -n 's/.* \([0-9][0-9.]*\).*/\1/p' "$OUT/version")
V="$FX-$(date -u +%Y%m%d)"
LIST="$OUT/firefox.json"
{
    printf '{"version":"%s","firefox":"%s","files":[' "$V" "$FX"
    sep=""
    for f in Image root.sqfs; do
        size=$(wc -c < "$OUT/$f" | tr -d ' ')
        sha=$(shasum -a 256 "$OUT/$f" | cut -d' ' -f1)
        name="fx/$f"
        printf '%s{"name":"%s","url":"https://boltos.dev/pkg/firefox/%s/%s","size":%s,"sha256":"%s"}' "$sep" "$name" "$V" "$f" "$size" "$sha"
        sep=","
    done
    printf ']}\n'
} > "$LIST"
cat "$LIST"
[ "${1:-}" = --dry-run ] && exit 0
export PATH="$ROOT/build/.tools/env/bin:$PATH"
cd "$ROOT/website"
for f in Image root.sqfs; do
    npx wrangler r2 object put "$BUCKET/firefox/$V/$f" --file "$OUT/$f" --remote
done
npx wrangler r2 object put "$BUCKET/firefox/firefox.json" --file "$LIST" --content-type application/json \
    --cache-control "no-cache" --remote
echo "published Firefox $V: https://boltos.dev/pkg/firefox/firefox.json"
