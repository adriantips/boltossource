#!/usr/bin/env bash
# ===========================================================================
#  Publish a new version of BoltOS as an update.
#
#    bash release.sh mini                  the Orange Pi edition
#    bash release.sh tv                    the television edition
#    bash release.sh desktop               the desktop
#
#  Each edition keeps its version in a file (mini/VERSION, tv/VERSION,
#  kernel/VERSION). This raises it -- the last number, unless told otherwise
#  -- builds the edition, and puts on boltos.dev:
#
#    /update/<edition>-<version>.bin    the kernel
#    /update/<edition>.json             which version is the newest, its size,
#                                       SHA-256, kind and what's new in it
#    /update/<edition>-history.json     every release so far, newest first
#
#  Every BoltOS asks for its .json when it starts and every few hours. A
#  quick update installs itself at the next start when automatic updates
#  are on; a big one waits until someone presses Update.
#
#  Options:
#    --big                 a big update: never installs by itself
#    --notes "a\nb"        what's new, a line each (default: the subjects of
#                          the edition's commits since the last release)
#    --version X.Y.Z       this version, instead of the next one
#    --minor, --major      raise the middle or first number instead
#    --channel NAME        publish as NAME.json instead of <edition>.json
#                          (a test channel nobody's machine asks for)
#    --site                then build and deploy boltos.dev, so the images it
#                          offers for download are this version too
#    --dry-run             build and write the description, upload nothing
#
#  The bucket is boltos-packages (website/wrangler.jsonc); uploads go
#  through wrangler, which has to be logged in to the Cloudflare account.
#  Afterwards, commit the VERSION file the script changed.
# ===========================================================================
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

TOOLS="${BOLTOS_TOOLS:-$ROOT/build/.tools/env/bin}"
[ -d "$TOOLS" ] && export PATH="$TOOLS:$PATH"
PYTHON="${BOLTOS_PYTHON:-$TOOLS/python3}"
[ -x "$PYTHON" ] || PYTHON=python3
BUCKET=boltos-packages
SITE=https://boltos.dev

usage() { sed -n '3,36p' "$0" | sed -E 's/^# ?//; s/^ //'; exit "${1:-0}"; }
[ $# -ge 1 ] || usage 1
EDITION="$1"; shift
case "$EDITION" in
mini)    NAME="BoltOS Mini"; VFILE=mini/VERSION;   DIRS=(mini) ;;
tv)      NAME="BoltOS TV";   VFILE=tv/VERSION;     DIRS=(tv build-tv.sh) ;;
desktop) NAME="BoltOS";      VFILE=kernel/VERSION; DIRS=(kernel drivers fs net mm libc include boot user) ;;
-h|--help) usage 0 ;;
*) echo "ERROR: which edition? mini, tv or desktop"; exit 1 ;;
esac

KIND=quick NOTES="" WANT="" BUMP=patch CHANNEL="$EDITION" DEPLOY_SITE=0 DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
    --big)      KIND=big ;;
    --notes)    NOTES="$2"; shift ;;
    --version)  WANT="$2"; shift ;;
    --minor)    BUMP=minor ;;
    --major)    BUMP=major ;;
    --channel)  CHANNEL="$2"; shift ;;
    --site)     DEPLOY_SITE=1 ;;
    --dry-run)  DRY=1 ;;
    -h|--help)  usage 0 ;;
    *) echo "ERROR: $1 isn't an option (bash release.sh --help)"; exit 1 ;;
    esac
    shift
done

OUT="build/release/$CHANNEL"
mkdir -p "$OUT"

# ---- what boltos.dev has now ------------------------------------------------
curl -fsS --max-time 20 "$SITE/update/$CHANNEL.json" -o "$OUT/published.json" 2>/dev/null || echo '{}' > "$OUT/published.json"
curl -fsS --max-time 20 "$SITE/update/$CHANNEL-history.json" -o "$OUT/history-old.json" 2>/dev/null || echo '[]' > "$OUT/history-old.json"

read -r PUBLISHED PREV_COMMIT < <("$PYTHON" - "$OUT/published.json" <<'PY'
import json, sys
try: m = json.load(open(sys.argv[1]))
except Exception: m = {}
print(m.get("version", "-"), m.get("commit", "-"))
PY
)
HAVE="$(tr -d ' \n' < "$VFILE")"

# ---- the new version ---------------------------------------------------------
VERSION="$("$PYTHON" - "$HAVE" "$PUBLISHED" "$BUMP" "$WANT" <<'PY'
import sys
have, pub, bump, want = sys.argv[1:5]
def parse(v):
    p = [int(x) for x in v.split(".")] if v not in ("", "-") else [0]
    return (p + [0, 0, 0])[:3]
if want:
    if parse(want) <= parse(pub): sys.exit("ERROR: %s isn't newer than boltos.dev's %s" % (want, pub))
    print(want); sys.exit()
h, p = parse(have), parse(pub)
if pub == "-" or h > p:                  # never published, or raised by hand: as it is
    print(".".join(map(str, h))); sys.exit()
i = {"major": 0, "minor": 1, "patch": 2}[bump]
p[i] += 1
for k in range(i + 1, 3): p[k] = 0
print(".".join(map(str, p)))
PY
)"
echo "== $NAME $VERSION ($KIND update; boltos.dev has $PUBLISHED, channel $CHANNEL)"
OLD_VFILE="$HAVE"
printf '%s\n' "$VERSION" > "$VFILE"
# A failed build or upload puts the version back as it was.
trap '[ "$(tr -d " \n" < "$VFILE")" = "$VERSION" ] && [ "${DONE:-0}" != 1 ] && printf "%s\n" "$OLD_VFILE" > "$VFILE"' EXIT

# ---- what's new -----------------------------------------------------------------
if [ -z "$NOTES" ]; then
    RANGE="HEAD~1..HEAD"
    if [ "$PREV_COMMIT" != "-" ] && git cat-file -e "$PREV_COMMIT^{commit}" 2>/dev/null; then RANGE="$PREV_COMMIT..HEAD"; fi
    NOTES="$(git log --no-merges --format=%s "$RANGE" -- "${DIRS[@]}" 2>/dev/null | head -12 | \
             sed -E 's/^(mini|tv|desktop|kernel|gui|shell|apps|net|website)( and [a-z]+)?: //' || true)"
    [ -n "$NOTES" ] || NOTES="Fixes and small improvements."
fi
NOTES="$(printf '%b' "$NOTES")"

# ---- build ---------------------------------------------------------------------
# A test channel's kernel asks for that channel's updates, not everyone's.
[ "$CHANNEL" != "$EDITION" ] && export BOLTOS_UPDATE_CHANNEL="$CHANNEL"
case "$EDITION" in
mini)
    # What boltos.dev hands out has no Unisoc firmware in it (see MINI.md):
    # a Pi keeps the copy its own kernel left on the card.
    BOLTOS_NO_FW=1 BOLTOS_BUILD_DIR=build/release-mini bash build-mini.sh
    KERNEL=build/release-mini/boltos.bin
    if [ "$CHANNEL" = mini ]; then
        BOLTOS_NO_FW=1 BOLTOS_BUILD_DIR=build/release-mini-release bash build-mini.sh release
        EXTRA_KERNEL=build/release-mini-release/boltos.bin
        # The card image boltos.dev/mini offers: the same kernel.
        gzip -9 -n -c build/release-mini/sdcard.img > iso/boltos-mini-sdcard.img.gz
    fi ;;
tv)
    bash build-tv.sh
    KERNEL=build/tv/kernel.bin ;;
desktop)
    bash build.sh
    KERNEL=build/kernel.bin ;;
esac
[ -f "$KERNEL" ] || { echo "ERROR: the build made no $KERNEL"; exit 1; }

# ---- the description -------------------------------------------------------------
describe() {   # describe CHANNEL KERNEL -> $OUT/<channel>.json and its history
    "$PYTHON" - "$1" "$2" "$EDITION" "$NAME" "$VERSION" "$KIND" "$NOTES" "$(git rev-parse --short HEAD 2>/dev/null || echo -)" \
                "$SITE" "$OUT/history-old.json" "$OUT" <<'PY'
import hashlib, json, os, sys, time
ch, kernel, edition, name, version, kind, notes, commit, site, hist_old, out = sys.argv[1:12]
data = open(kernel, "rb").read()
m = {
    "edition": edition, "name": name, "channel": ch, "version": version,
    "date": time.strftime("%Y-%m-%d"), "kind": kind,
    "url": "%s/update/%s-%s.bin" % (site, ch, version),
    "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
    "notes": notes.strip(), "commit": commit,
}
json.dump(m, open(os.path.join(out, ch + ".json"), "w"), indent=2)
try: hist = json.load(open(hist_old)) if ch == os.path.basename(out) else []
except Exception: hist = []
hist = [h for h in hist if h.get("version") != version]
hist.insert(0, {k: m[k] for k in ("version", "date", "kind", "size", "notes")})
json.dump(hist[:40], open(os.path.join(out, ch + "-history.json"), "w"), indent=2)
print("   %s.json: %s, %d bytes, sha256 %s..." % (ch, version, len(data), m["sha256"][:16]))
PY
}
describe "$CHANNEL" "$KERNEL"
[ -n "${EXTRA_KERNEL:-}" ] && describe "$CHANNEL-release" "$EXTRA_KERNEL"

if [ "$DRY" = 1 ]; then
    echo "-- dry run: nothing uploaded; the description is $OUT/$CHANNEL.json"
    printf '%s\n' "$OLD_VFILE" > "$VFILE"
    DONE=1
    exit 0
fi

# ---- upload: the kernel first, the description last -------------------------------
cd website
WR=(npx --yes wrangler r2 object put)
put() {   # put KEY FILE TYPE
    "${WR[@]}" "$BUCKET/update/$1" --file "$ROOT/$2" --content-type "$3" --remote > /dev/null
    echo "   uploaded update/$1"
}
put "$CHANNEL-$VERSION.bin" "$KERNEL" application/octet-stream
[ -n "${EXTRA_KERNEL:-}" ] && put "$CHANNEL-release-$VERSION.bin" "$EXTRA_KERNEL" application/octet-stream
[ -n "${EXTRA_KERNEL:-}" ] && put "$CHANNEL-release.json" "$OUT/$CHANNEL-release.json" application/json
put "$CHANNEL-history.json" "$OUT/$CHANNEL-history.json" application/json
put "$CHANNEL.json" "$OUT/$CHANNEL.json" application/json
cd "$ROOT"
DONE=1

if [ "$DEPLOY_SITE" = 1 ]; then
    ( cd website && npm run build && npx --yes wrangler deploy )
fi

echo "== $NAME $VERSION is on $SITE/update/$CHANNEL.json"
echo "   commit $VFILE${EXTRA_KERNEL:+ and iso/boltos-mini-sdcard.img.gz}"
