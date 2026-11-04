#!/usr/bin/env bash
# ci_watchdog.sh MINUTES COMMAND...: runs COMMAND; if it is still running after MINUTES, prints
# every process still running under it (so a hung test names itself in the CI log), then kills
# it and fails. A job-level timeout-minutes cancel discards the step's log, so a hang there
# leaves nothing to go on.
set -u
minutes=$1
shift
"$@" &
pid=$!
deadline=$((SECONDS + minutes * 60))
while kill -0 "$pid" 2>/dev/null; do
    if ((SECONDS >= deadline)); then
        echo "ci_watchdog: '$*' still running after ${minutes} min; processes:" >&2
        ps -axo pid,ppid,etime,command >&2
        pkill -TERM -P "$pid" 2>/dev/null
        kill -TERM "$pid" 2>/dev/null
        sleep 5
        pkill -KILL -P "$pid" 2>/dev/null
        kill -KILL "$pid" 2>/dev/null
        exit 124
    fi
    sleep 5
done
wait "$pid"
