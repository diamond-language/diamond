#!/usr/bin/env bash
# The example applications that build on the repository's own cuts and so are not covered by `make test-examples`: guestbook,
# project_board, library and chat. Each gets the cuts from packages/ (tools/install_local_cuts.sh, as its README says), then its
# documented setup, then a real check: a smoke test, or for library requests to every documented route. They broke silently when
# a package changed under them before anything ran them.
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
export DIAMOND_NO_CACHE=1
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true; }
trap cleanup EXIT

install_cuts() { bash "$root/tools/install_local_cuts.sh" . >/dev/null; }

echo "== guestbook"
(cd examples/guestbook && install_cuts && "$diamond" smoke_test.di >/dev/null)

echo "== project_board"
(cd examples/project_board && install_cuts && DIAMOND_BIN="$diamond" bash compile_views.sh >/dev/null \
    && DIAMOND_ENV=test "$diamond" setup_db.di >/dev/null && DIAMOND_ENV=test "$diamond" smoke_test.di 2>&1 | grep -q smoke_test.passed)

echo "== chat"
(cd examples/chat && install_cuts && "$diamond" smoke_test.di 2>&1 | grep -q "chat smoke test passed")

echo "== library"
(
    cd examples/library
    install_cuts
    DIAMOND_BIN="$diamond" bash compile_views.sh >/dev/null
    DIAMOND_ENV=test "$diamond" setup_db.di >/dev/null
    DIAMOND_ENV=test "$diamond" app.di >"$(mktemp)" 2>&1 &
    server_pid=$!
    ready=0
    for _ in $(seq 1 100); do
        if (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null; then ready=1; break; fi
        sleep 0.2
    done
    [[ "$ready" == 1 ]] || { echo "library did not start" >&2; exit 1; }
    for path in / /authors /books /books/available /authors/1 /books/1; do
        status="$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:18080$path")"
        [[ "$status" == 200 ]] || { echo "library: GET $path answered $status" >&2; exit 1; }
    done
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://127.0.0.1:18080/no-such-path)" == 404 ]]
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
)
server_pid=""

echo "example apps passed"
