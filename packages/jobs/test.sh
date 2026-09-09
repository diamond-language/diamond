#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-jobs-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/jobs\"
db = SQLite3.open(\":memory:\")
db.execute(\"CREATE TABLE jobs (id INTEGER PRIMARY KEY, kind TEXT NOT NULL, payload TEXT NOT NULL, queue TEXT NOT NULL DEFAULT 'default', status TEXT NOT NULL DEFAULT 'pending', run_at INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL DEFAULT 5, last_error TEXT, locked_at INTEGER, created_at INTEGER NOT NULL, finished_at INTEGER)\")
db.execute(\"CREATE TABLE recurring_jobs (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, kind TEXT NOT NULL, payload TEXT NOT NULL, queue TEXT NOT NULL DEFAULT 'default', every_seconds INTEGER NOT NULL, next_run_at INTEGER NOT NULL, created_at INTEGER NOT NULL)\")
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

# --- enqueue creates a pending row; run_once claims and succeeds it;
# a second run_once with nothing left due returns false ---
actual="$(run_case '
id = Jobs.enqueue(db, kind: "noop", args: {})
def noop_handler(args)
end
handlers = {"noop": noop_handler}
first = Jobs::Worker.run_once(db, handlers: handlers)
status = db.query("SELECT status FROM jobs WHERE id = ?", [id])[0]["status"]
second = Jobs::Worker.run_once(db, handlers: handlers)
"#{first} #{status} #{second}"
')"
assert_eq "$actual" "true succeeded false"

# --- a delayed job (run_at in the future) is not claimed early ---
actual="$(run_case '
Jobs.enqueue(db, kind: "noop", args: {}, run_at: Time.now().to_i() + 3600)
def noop_handler(args)
end
Jobs::Worker.run_once(db, handlers: {"noop": noop_handler})
')"
assert_eq "$actual" "false"

# --- a handler that raises retries with the attempt count/error
# recorded and run_at pushed into the future, up to max_attempts,
# then fails permanently ---
actual="$(run_case '
def boom_handler(args)
  raise ArgumentError.new("boom")
end
handlers = {"boom": boom_handler}
id = Jobs.enqueue(db, kind: "boom", args: {}, max_attempts: 2)
Jobs::Worker.run_once(db, handlers: handlers)
after_first = db.query("SELECT * FROM jobs WHERE id = ?", [id])[0]
db.execute("UPDATE jobs SET run_at = ? WHERE id = ?", [Time.now().to_i(), id])
Jobs::Worker.run_once(db, handlers: handlers)
after_second = db.query("SELECT * FROM jobs WHERE id = ?", [id])[0]
"#{after_first["status"]} #{after_first["attempts"]} #{after_first["run_at"] > Time.now().to_i()} #{after_second["status"]} #{after_second["attempts"]} #{after_second["last_error"]}"
')"
assert_eq "$actual" "pending 1 true failed 2 boom"

# --- no handler registered for a job's own kind fails it immediately,
# without ever retrying ---
actual="$(run_case '
id = Jobs.enqueue(db, kind: "mystery", args: {})
Jobs::Worker.run_once(db, handlers: {})
row = db.query("SELECT * FROM jobs WHERE id = ?", [id])[0]
"#{row["status"]} #{row["attempts"]} #{row["last_error"]}"
')"
assert_eq "$actual" "failed 1 no handler registered for kind 'mystery'"

# --- queue filtering: a worker restricted to one queue never claims
# a job enqueued on a different queue ---
actual="$(run_case '
Jobs.enqueue(db, kind: "noop", args: {}, queue: "reports")
def noop_handler(args)
end
handlers = {"noop": noop_handler}
wrong_queue = Jobs::Worker.run_once(db, handlers: handlers, queues: ["default"])
right_queue = Jobs::Worker.run_once(db, handlers: handlers, queues: ["reports"])
"#{wrong_queue} #{right_queue}"
')"
assert_eq "$actual" "false true"

# --- schedule_recurring is idempotent by name -- calling it again
# with the same name is a no-op, safe to call unconditionally from an
# app's own boot path ---
actual="$(run_case '
first_id = Jobs.schedule_recurring(db, name: "heartbeat", kind: "heartbeat", every_seconds: 60)
second_id = Jobs.schedule_recurring(db, name: "heartbeat", kind: "heartbeat", every_seconds: 60)
count = db.query("SELECT COUNT(*) AS c FROM recurring_jobs")[0]["c"]
"#{first_id == second_id} #{count}"
')"
assert_eq "$actual" "true 1"

# --- a recurring job does not fire until its own interval has
# actually elapsed, and advances next_run_at from "now" (not the old
# next_run_at) so a worker that was down for a while does not fire a
# burst of catch-up jobs on the next tick ---
actual="$(run_case '
Jobs.schedule_recurring(db, name: "heartbeat", kind: "heartbeat", every_seconds: 60)
before_count = db.query("SELECT COUNT(*) AS c FROM jobs")[0]["c"]
Jobs::Worker.tick_recurring(db, Time.now().to_i())
not_due_yet = db.query("SELECT COUNT(*) AS c FROM jobs")[0]["c"] == before_count
Jobs::Worker.tick_recurring(db, Time.now().to_i() + 61)
due_now = db.query("SELECT COUNT(*) AS c FROM jobs")[0]["c"] == before_count + 1
next_run_at = db.query("SELECT next_run_at FROM recurring_jobs WHERE name = ?", ["heartbeat"])[0]["next_run_at"]
advanced_from_now = next_run_at > Time.now().to_i() + 100
"#{not_due_yet} #{due_now} #{advanced_from_now}"
')"
assert_eq "$actual" "true true true"

echo "$count jobs tests passed"
