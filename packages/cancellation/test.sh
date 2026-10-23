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
timeout 30 "$diamond" test/cancellation_test.d6
"$diamond" build test/cancellation_test.d6 -o "$work/cancellation-test" > "$work/build.log"
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
check_signal_wait "$diamond" test/signal_test.d6
"$diamond" build test/signal_test.d6 -o "$work/signal-test" > "$work/signal-build.log"
check_signal_wait "$work/signal-test"
echo "cancellation signal tests passed"
"$diamond" test/socket_driver.d6 "$diamond" test/socket_test.d6
"$diamond" build test/socket_test.d6 -o "$work/socket-test" > "$work/socket-build.log"
"$diamond" test/socket_driver.d6 "$work/socket-test"
"$diamond" test/connect_driver.d6 "$diamond" test/connect_test.d6
"$diamond" build test/connect_test.d6 -o "$work/connect-test" > "$work/connect-build.log"
"$diamond" test/connect_driver.d6 "$work/connect-test"
"$diamond" test/dns_driver.d6 "$diamond" test/dns_test.d6
"$diamond" build test/dns_test.d6 -o "$work/dns-test" > "$work/dns-build.log"
"$diamond" test/dns_driver.d6 "$work/dns-test"
"$diamond" test/tls_driver.d6 "$diamond" test/tls_test.d6
"$diamond" build test/tls_test.d6 -o "$work/tls-test" > "$work/tls-build.log"
"$diamond" test/tls_driver.d6 "$work/tls-test"
