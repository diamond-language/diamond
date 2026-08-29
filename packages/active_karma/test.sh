#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-active-karma-package` from the repo root, which sets this up
# already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/active_karma\"
$script"
}

assert_eq() {
    local actual="$1" expected="$2"
    if [[ "$actual" != "$expected" ]]; then
        echo "expected '$expected', got '$actual'" >&2
        exit 1
    fi
    count=$((count + 1))
}

# --- PersonaState.default is the zero-signal baseline ---
actual="$(run_case '
s = ActiveKarma::PersonaState.default()
"#{s.normal?()} #{s.write_allowed?()} #{s.visible?()} #{s.locked?()}"
')"
assert_eq "$actual" "true true true false"

# --- a spam signal limits trust and throttles writes ---
actual="$(run_case '
events = [{"event_type": ActiveKarma::Signal::SPAM_DETECTED, "created_at": 1}]
s = ActiveKarma::Projector.project(events)
"#{s.limited?()} #{s.write_throttled?()}"
')"
assert_eq "$actual" "true true"

# --- an abuse signal blocks and shadows ---
actual="$(run_case '
events = [{"event_type": ActiveKarma::Signal::ABUSE_DETECTED, "created_at": 1}]
s = ActiveKarma::Projector.project(events)
"#{s.blocked?()} #{s.write_denied?()} #{s.shadow?()}"
')"
assert_eq "$actual" "true true true"

# --- a rate-limit signal never un-denies an already-denied write
# permission (denied is a stronger prior state a later throttle
# shouldn't loosen) ---
actual="$(run_case '
events = [
  {"event_type": ActiveKarma::Signal::ABUSE_DETECTED, "created_at": 1},
  {"event_type": ActiveKarma::Signal::RATE_LIMITED, "created_at": 2},
]
s = ActiveKarma::Projector.project(events)
s.write_denied?()
')"
assert_eq "$actual" "true"

# --- events are folded oldest-first regardless of Array order ---
actual="$(run_case '
events = [
  {"event_type": ActiveKarma::Signal::MANUALLY_BLOCKED, "created_at": 5},
  {"event_type": ActiveKarma::Signal::MANUALLY_CLEARED, "created_at": 10},
]
s = ActiveKarma::Projector.project(events)
s.normal?()
')"
assert_eq "$actual" "true"

# --- a compromised lock is idempotent (two signals, one lock entry) ---
actual="$(run_case '
events = [
  {"event_type": ActiveKarma::Signal::COMPROMISED, "created_at": 1},
  {"event_type": ActiveKarma::Signal::COMPROMISED, "created_at": 2},
]
s = ActiveKarma::Projector.project(events)
"#{s.compromised?()} #{s.locks().length()}"
')"
assert_eq "$actual" "true 1"

# --- verification clears its own lock and, once no locks and trust is
# still normal, restores write access ---
actual="$(run_case '
events = [
  {"event_type": ActiveKarma::Signal::VERIFICATION_REQUIRED, "created_at": 1},
  {"event_type": ActiveKarma::Signal::VERIFICATION_PROVIDED, "created_at": 2},
]
s = ActiveKarma::Projector.project(events)
"#{s.verification_required?()} #{s.write_allowed?()}"
')"
assert_eq "$actual" "false true"

# --- manually_cleared resets every dimension, including locks ---
actual="$(run_case '
events = [
  {"event_type": ActiveKarma::Signal::COMPROMISED, "created_at": 1},
  {"event_type": ActiveKarma::Signal::MANUALLY_CLEARED, "created_at": 2},
]
s = ActiveKarma::Projector.project(events)
"#{s.normal?()} #{s.locked?()}"
')"
assert_eq "$actual" "true false"

# --- can_write?/cannot_write? mirror write_permission ---
actual="$(run_case '
events = [{"event_type": ActiveKarma::Signal::ABUSE_DETECTED, "created_at": 1}]
s = ActiveKarma::Projector.project(events)
"#{s.can_write?()} #{s.cannot_write?()}"
')"
assert_eq "$actual" "false true"

# --- to_h returns every dimension as a plain Hash ---
actual="$(run_case '
h = ActiveKarma::PersonaState.default().to_h()
"#{h["trust_level"]} #{h["write_permission"]} #{h["visibility"]} #{h["locks"].length()}"
')"
assert_eq "$actual" "normal allowed normal 0"

# --- Signal.valid?/.namespaced accept a bare or already-namespaced flag ---
actual="$(run_case '
"#{ActiveKarma::Signal.valid?("spam_detected")} #{ActiveKarma::Signal.valid?("karma.spam_detected")} #{ActiveKarma::Signal.valid?("nope")}"
')"
assert_eq "$actual" "true true false"

actual="$(run_case '
ActiveKarma::Signal.namespaced("abuse_detected")
')"
assert_eq "$actual" "karma.abuse_detected"

echo "$count active_karma tests passed"
