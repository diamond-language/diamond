#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
diamond_bin="$repo_dir/build/diamond"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT

cat > "$test_dir/app.di" <<'EOF'
"built from application directory"
EOF

cd "$test_dir"
"$diamond_bin" build app.di
test -x "$test_dir/app"
output="$("$test_dir/app")"
test "$output" = "built from application directory"
printf '%s\n' "build from application directory passed"
