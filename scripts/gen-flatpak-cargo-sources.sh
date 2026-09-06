#!/usr/bin/env bash
# Regenerate packaging/flatpak/cargo-sources.json from Cargo.lock.
#
# Flathub builds have no network, so the Flatpak cannot let cargo fetch
# crates: every one has to be a declared source with a checksum, which is
# what this file is (513 crates + their .cargo-checksum.json + the cargo
# config that redirects crates-io at the vendor directory).
#
# Run it after any dependency change, and commit the result:
#   scripts/gen-flatpak-cargo-sources.sh
#
# The generator is upstream's (flatpak/flatpak-builder-tools, MIT), pinned
# to a commit and checked against its hash rather than trusted from a
# moving branch — it is a script this repo executes. It needs aiohttp and
# tomlkit, supplied by uv so nothing is installed on the host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

COMMIT=f03a673abe6ce189cea1c2857e2b44af2dd79d1f   # 2025-08-16, "cargo: unwrap tomldoc"
SHA256=b373c8ab1a05378ec5d8ed0645c7b127bcec7d2f7a1798694fbc627d570d856c
URL="https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/$COMMIT/cargo/flatpak-cargo-generator.py"
TOOL="$ROOT/build/flatpak-tools/flatpak-cargo-generator.py"
OUT="$ROOT/packaging/flatpak/cargo-sources.json"

command -v uv >/dev/null || { echo "uv not found — https://docs.astral.sh/uv/" >&2; exit 1; }
mkdir -p "$(dirname "$TOOL")"
if ! [ -f "$TOOL" ] || ! echo "$SHA256  $TOOL" | sha256sum -c --status; then
  echo "==> fetching flatpak-cargo-generator ($COMMIT)"
  curl -fsSL -o "$TOOL" "$URL"
  echo "$SHA256  $TOOL" | sha256sum -c --status || {
    echo "gen-flatpak-cargo-sources.sh: the generator's hash does not match; refusing to run it" >&2
    rm -f "$TOOL"; exit 1; }
fi

echo "==> generating $OUT from Cargo.lock"
uv run --quiet --python 3.12 --with aiohttp --with tomlkit --with PyYAML \
  python "$TOOL" Cargo.lock -o "$OUT"

python3 - "$OUT" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
kinds = {}
for e in s:
    kinds[e["type"]] = kinds.get(e["type"], 0) + 1
crates = [e for e in s if e["type"] == "archive"]
assert crates, "no crate archives generated"
assert all(e["url"].startswith("https://") and e.get("sha256") for e in crates), \
    "a crate source has no https url or no checksum"
cfg = [e for e in s if e.get("dest") == "cargo" and e.get("dest-filename", "").startswith("config")]
assert len(cfg) == 1, f"expected exactly one cargo config entry, got {len(cfg)}"
print(f"{len(s)} sources: {kinds}")
EOF
