#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-diamond}"
package_dir="$(cd "$(dirname "$0")" && pwd)"
viewer="$package_dir/bin/diamond-log"
fixture="$(mktemp)"
output="$(mktemp)"
cleanup() { rm -f "$fixture" "$output"; }
trap cleanup EXIT

cat >"$fixture" <<'EOF'
{"timestamp":"2026-08-25T12:00:00-0700","level":"info","tag":"project_board","message":"request.completed","status":200,"duration_ms":0.4821,"request_id":"abc123","method":"GET","path":"/projects","custom":{"ok":true}}
{"timestamp":"2026-08-25T12:00:01-0700","level":"debug","tag":"project_board","message":"database.query.completed","query_id":"q1","phase":"completed","operation":"query","sql":"SELECT * FROM projects WHERE id = ?","bind_count":1,"rows":1,"duration_ms":0.071,"request_id":"abc123","method":"GET","path":"/projects/1"}
not json
42
EOF

DIAMOND_BIN="$diamond" "$viewer" "$fixture" >"$output"
grep -Fq '2026-08-25T12:00:00-0700 INFO [project_board] request.completed request_id="abc123" method="GET" path="/projects" status=200 duration_ms=0.4821 custom={"ok":true}' "$output"
grep -Fq 'DEBUG [project_board] database.query.completed request_id="abc123" method="GET" path="/projects/1" duration_ms=0.071 query_id="q1" phase="completed" operation="query" sql="SELECT * FROM projects WHERE id = ?" bind_count=1 rows=1' "$output"
grep -Fq '[unparsed] not json' "$output"
grep -Fq '[unparsed] 42' "$output"

if DIAMOND_BIN="$diamond" "$viewer" --strict "$fixture" >/dev/null; then
  echo "strict mode accepted malformed input" >&2
  exit 1
fi

head -n 1 "$fixture" | DIAMOND_BIN="$diamond" "$viewer" >"$output"
grep -Fq 'request.completed' "$output"

help="$(DIAMOND_BIN="$diamond" "$viewer" --help)"
[[ "$help" == 'usage: diamond-log [--strict] [log.ndjson]' ]]

echo "7 log viewer tests passed"
