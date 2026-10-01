#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-diamond}"
work="$(mktemp -d)"
signal_pid=""
cleanup() {
    if [[ -n "$signal_pid" ]]; then
        kill -TERM "$signal_pid" 2>/dev/null || true
        wait "$signal_pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT
export DIAMOND_NO_CACHE=1
timeout 30 "$diamond" test/cancellation_test.di
"$diamond" build test/cancellation_test.di -o "$work/cancellation-test" > "$work/build.log"
timeout 30 "$work/cancellation-test"

# Signal handlers run at VM instruction boundaries, including while a token
# waits with no channel traffic or deadline. Handshake after handler installation.
check_signal_wait() {
    timeout --kill-after=2 10 "$@" > "$work/signal.log" 2>&1 &
    signal_pid=$!
    local ready=false
    for ((attempt = 0; attempt < 100; attempt++)); do
        if grep -qx ready "$work/signal.log"; then ready=true; break; fi
        if ! kill -0 "$signal_pid" 2>/dev/null; then break; fi
        sleep 0.05
    done
    if [[ "$ready" != true ]]; then
        cat "$work/signal.log" >&2
        echo "cancellation signal test did not become ready" >&2
        return 1
    fi
    kill -TERM "$signal_pid"
    # timeout implementations disagree on the status after a forwarded signal.
    wait "$signal_pid" || true
    signal_pid=""
    if [[ "$(cat "$work/signal.log")" != $'ready\ncancelled' ]]; then
        cat "$work/signal.log" >&2
        echo "cancellation signal test did not cancel the wait" >&2
        return 1
    fi
}
check_signal_wait "$diamond" test/signal_test.di
"$diamond" build test/signal_test.di -o "$work/signal-test" > "$work/signal-build.log"
check_signal_wait "$work/signal-test"
echo "cancellation signal tests passed"
python3 test/socket_test.py "$diamond" test/socket_test.di
"$diamond" build test/socket_test.di -o "$work/socket-test" > "$work/socket-build.log"
python3 test/socket_test.py "$work/socket-test"
python3 test/connect_test.py "$diamond" test/connect_test.di
"$diamond" build test/connect_test.di -o "$work/connect-test" > "$work/connect-build.log"
python3 test/connect_test.py "$work/connect-test"
python3 test/dns_test.py "$diamond" test/dns_test.di
"$diamond" build test/dns_test.di -o "$work/dns-test" > "$work/dns-build.log"
python3 test/dns_test.py "$work/dns-test"
python3 test/tls_test.py "$diamond" test/tls_test.di
"$diamond" build test/tls_test.di -o "$work/tls-test" > "$work/tls-build.log"
python3 test/tls_test.py "$work/tls-test"
