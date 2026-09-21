#!/usr/bin/env bash
# Regenerates docs/contracts/mouse_slot_vectors.json from the C encoders
# (docs/DONGLE_MOUSE_CONTRACT.md). Host compiler only.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -DTEST_HOST -I main/comm/rf -I test scripts/gen/mouse_slot_vectors.c main/comm/rf/rf_pairing.c -o "$tmp/gen"
"$tmp/gen" | python3 -m json.tool --indent 2 > docs/contracts/mouse_slot_vectors.json
echo "wrote docs/contracts/mouse_slot_vectors.json"
