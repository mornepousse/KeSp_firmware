#!/usr/bin/env bash
# Regenerates docs/contracts/rf_trace_vectors.json from the C codec
# (main/comm/rf/rf_trace.c — the rf-scope trace format). Host compiler only.
# test_rf_trace fails until the committed file equals this output; the
# host tool's pytest (tools/rf_scope/tests/test_codec.py) decodes it.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
cc -std=c11 -DTEST_HOST -I main/comm/rf -I main/comm/cdc -I test \
   scripts/gen/rf_trace_vectors.c main/comm/rf/rf_trace.c main/comm/cdc/cdc_binary_protocol.c \
   -o "$tmp/gen"
"$tmp/gen" > docs/contracts/rf_trace_vectors.json
python3 -m json.tool docs/contracts/rf_trace_vectors.json > /dev/null   # must be valid JSON
echo "wrote docs/contracts/rf_trace_vectors.json"
