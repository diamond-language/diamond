#!/usr/bin/env bash
# Build and test Diamond on this machine and write a report to send back.
#
# Meant to be run by someone who isn't a Diamond developer:
#
#     bash tools/platform_report/platform_report.sh
#
# It works in a throwaway copy of this checkout, asks before installing anything, keeps
# going when a step fails, and writes one text file (on the Desktop when there is one)
# that holds everything a maintainer needs. Plain POSIX tools only; written for the bash
# 3.2 and BSD userland that macOS ships, so it runs before any Diamond exists to run it.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IS_MAC=0
[ "$(uname -s)" = "Darwin" ] && IS_MAC=1
BREW_PACKAGES="coreutils gnu-sed bash ripgrep openssl@3 sqlite postgresql@16 mariadb-connector-c"
# "benchmark:in-process repeats", a representative slice of bench/*.d6.
BENCHMARKS="fibonacci:15 int_arithmetic:15 dispatch_monomorphic:15 dispatch_polymorphic:12
closures:25 array_ops:30 hash_ops:40 string_ops:40 struct_field_access:15 regexp_match:15
tensor_matmul:20 exception_handling:12 fiber_switch:10 channel_message_passing:15
thread_pool:15 dispatch_megamorphic:12 object_hydration:8 iterator_blocks:15
case_pattern_matching:15 hash_ivar_construct:15"
# "make target|rough duration shown so a quiet step doesn't look stuck"
TEST_TARGETS="test|the main suite: 5-15 minutes
test-api|about a minute
test-fibers|about a minute
test-facet|1-3 minutes
test-aot-cache|about a minute
test-aot-kit|1-2 minutes
test-lsp|about a minute
test-repl|under a minute
test-examples|every standalone example, interpreted and built: 3-6 minutes"

REPORT="$(mktemp "${TMPDIR:-/tmp}/diamond-report-lines.XXXXXX")"
STEP_OUTPUT="$(mktemp "${TMPDIR:-/tmp}/diamond-report-step.XXXXXX")"
WORK_PARENT=""
HOGS=""
STEP_STATUS=0
STEP_SECONDS=0
MAKE_ARGS=()

cleanup() {
    for pid in $HOGS; do kill "$pid" 2>/dev/null; done
    rm -f "$REPORT" "$STEP_OUTPUT"
    [ -n "$WORK_PARENT" ] && rm -rf "$WORK_PARENT"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

say() { printf '%s\n' "$*"; }
record() { printf '%s\n' "$*" >> "$REPORT"; }

section() {
    say
    say "== $1 =="
    record
    record "## $1"
}

# The last N lines of the file named by $1 (default: the last step's output).
tail_of() { tail -n "${2:-40}" "${1:-$STEP_OUTPUT}"; }

# Runs a command with an optional time limit ($1 seconds, 0 for none), keeping its
# combined output in $STEP_OUTPUT, its exit status in $STEP_STATUS, and the seconds it took
# in $STEP_SECONDS. Anything nonzero counts as a failure; a command that runs out of time
# exits 124. Its stdin is closed so a step cannot swallow a here-document loop's input.
step() {
    local limit="$1"
    shift
    local started=$SECONDS
    if [ "$limit" -gt 0 ] && [ -n "$TIMEOUT_CMD" ]; then
        "$TIMEOUT_CMD" "$limit" "$@" > "$STEP_OUTPUT" 2>&1 < /dev/null
    else
        "$@" > "$STEP_OUTPUT" 2>&1 < /dev/null
    fi
    STEP_STATUS=$?
    [ "$STEP_STATUS" -eq 124 ] && printf '\n[timed out]\n' >> "$STEP_OUTPUT"
    STEP_SECONDS=$((SECONDS - started))
}

# Output of a quick command, or nothing when it fails.
capture() {
    local out
    out="$("$@" 2>/dev/null)" || return 0
    printf '%s' "$out"
}

ask() {
    local answer
    printf '%s [y/N] ' "$1"
    read -r answer || return 1
    case "$answer" in y | Y | yes | YES | Yes) return 0 ;; *) return 1 ;; esac
}

fact() {
    local value="$2"
    [ -z "$value" ] && value="unknown"
    say "  $1: $value"
    record "$1: $value"
}

cpu_count() {
    if [ "$IS_MAC" -eq 1 ]; then sysctl -n hw.ncpu; else getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4; fi
}

machine_info() {
    section "Machine"
    fact "Date" "$(date '+%Y-%m-%dT%H:%M:%S')"
    fact "System" "$(uname -srm)"
    fact "Architecture" "$(uname -m)"
    if [ "$IS_MAC" -eq 1 ]; then
        local lowpower
        lowpower="$(pmset -g 2>/dev/null | awk '/lowpowermode/ {print $2}')"
        fact "macOS" "$(capture sw_vers -productVersion)"
        fact "Chip" "$(capture sysctl -n machdep.cpu.brand_string)"
        fact "CPU cores" "$(capture sysctl -n hw.ncpu)"
        fact "Performance cores" "$(capture sysctl -n hw.perflevel0.physicalcpu)"
        fact "Efficiency cores" "$(capture sysctl -n hw.perflevel1.physicalcpu)"
        fact "Memory bytes" "$(capture sysctl -n hw.memsize)"
        fact "Power" "$(pmset -g batt 2>/dev/null | head -n 1)"
        fact "Low power mode" "$lowpower"
    else
        fact "CPU cores" "$(cpu_count)"
    fi
    fact "C compiler" "$(cc --version 2>/dev/null | head -n 1)"
    fact "Diamond commit" "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)"
}

# Checks tools a person must install themselves. Returns 1 to stop.
prerequisites() {
    section "Prerequisites"
    if [ "$IS_MAC" -eq 1 ] && ! xcode-select -p > /dev/null 2>&1; then
        say "Apple's command line developer tools are missing. Install them with:"
        say "    xcode-select --install"
        say "then run this script again."
        record "Xcode command line tools: missing (stopped)"
        return 1
    fi
    if [ "$IS_MAC" -eq 1 ] && ! command -v brew > /dev/null 2>&1; then
        say "Homebrew is missing. Install it by following https://brew.sh, then run this"
        say "script again. It installs the libraries Diamond builds against."
        record "Homebrew: missing (stopped)"
        return 1
    fi
    if [ "$IS_MAC" -eq 0 ]; then
        say "Not macOS: skipping Homebrew; the build uses this system's own libraries."
        record "Homebrew: not used (not macOS)"
        return 0
    fi
    local installed missing="" package
    installed=" $(brew list --formula -1 2>/dev/null | tr '\n' ' ') "
    for package in $BREW_PACKAGES; do
        case "$installed" in *" $package "*) ;; *) missing="$missing $package" ;; esac
    done
    missing="${missing# }"
    if [ -n "$missing" ]; then
        say "Diamond needs these Homebrew packages:"
        say "    $missing"
        if ! ask "Install them now with Homebrew?"; then
            say "OK, stopping. Run \`brew install $missing\` and try again."
            record "Homebrew packages: declined install (stopped)"
            return 1
        fi
        # shellcheck disable=SC2086
        step 0 brew install $missing
        record "brew install $missing: exit $STEP_STATUS in ${STEP_SECONDS}s"
        if [ "$STEP_STATUS" -ne 0 ]; then
            say "Homebrew couldn't install everything; the report records why."
            tail_of >> "$REPORT"
            return 1
        fi
    fi
    record "Homebrew packages: present"
    say "  everything needed is installed"
    return 0
}

# The test suite writes a bytecode cache (.d6c, about 4 MB each because every one embeds
# the prelude) next to each of its ~1,700 test programs: roughly 6-7 GiB at the peak of
# `make test`, plus the build itself. Leave headroom so a nearly full disk is reported up
# front rather than as a dozen unrelated-looking compiler errors.
MIN_FREE_KIB=$((14 * 1024 * 1024))

free_kib() { df -Pk "$1" 2> /dev/null | awk 'NR == 2 {print $4}'; }

# Prints the directory to work in: the system temp directory when it has room, else the
# home directory (a RAM-backed /tmp is common on Linux). Prints nothing when neither does.
check_free_space() {
    section "Disk space" >&2
    local temp="${TMPDIR:-/tmp}" candidate free chosen=""
    temp="${temp%/}"
    for candidate in "$temp" "$HOME"; do
        free="$(free_kib "$candidate")"
        free="${free:-0}"
        record "free space in $candidate: $((free / 1024 / 1024)) GiB"
        say "  $((free / 1024 / 1024)) GiB free in $candidate" >&2
        if [ -z "$chosen" ] && [ "$free" -ge "$MIN_FREE_KIB" ]; then chosen="$candidate"; fi
    done
    if [ -n "$chosen" ]; then
        say "  working in $chosen" >&2
        record "working directory: $chosen"
        printf '%s' "$chosen"
        return 0
    fi
    say "  That is less than the $((MIN_FREE_KIB / 1024 / 1024)) GiB the test suite needs." >&2
    say "  Free some space (or empty the Trash) and run this again." >&2
    record "not enough free disk space (stopped)"
}

# PATH and make arguments matching the macOS CI job.
build_environment() {
    export DIAMOND_CASE_RUNNER=subprocess
    MAKE_ARGS=()
    if [ "$IS_MAC" -eq 1 ]; then
        local includes="" libs="" pkg
        p() { brew --prefix "$1"; }
        PATH="$(p coreutils)/libexec/gnubin:$(p gnu-sed)/libexec/gnubin:$(p bash)/bin:$PATH"
        export PATH
        for pkg in openssl@3 sqlite postgresql@16 mariadb-connector-c; do
            includes="$includes -I$(p "$pkg")/include"
            libs="$libs -L$(p "$pkg")/lib"
        done
        includes="$includes -I$(p mariadb-connector-c)/include/mariadb"
        libs="$libs -L$(p mariadb-connector-c)/lib/mariadb"
        MAKE_ARGS=(CC=clang LDLIBS_CRYPT= "CPPFLAGS_EXTRA=${includes# }" "LDFLAGS_EXTRA=${libs# }")
    fi
    TIMEOUT_CMD=""
    if command -v timeout > /dev/null 2>&1; then TIMEOUT_CMD=timeout
    elif command -v gtimeout > /dev/null 2>&1; then TIMEOUT_CMD=gtimeout; fi
}

# The same exclusions as the macOS CI job: BCrypt (no crypt_gensalt on macOS) and cases
# that exercise the glibc-only x86-64 JIT. Prints how many files it removed.
exclude_known_gaps() {
    local cases="$1/tests/cases" removed=0 name suffix file stem
    if [ "$IS_MAC" -eq 1 ]; then
        for name in active_record_secure_password bcrypt; do
            for suffix in .d6 .expected; do
                if [ -e "$cases/$name$suffix" ]; then
                    rm -f "$cases/$name$suffix"
                    removed=$((removed + 1))
                fi
            done
        done
    fi
    if [ "$IS_MAC" -eq 1 ] || [ "$(uname -m)" != "x86_64" ]; then
        for file in "$cases"/*.env; do
            [ -e "$file" ] || continue
            grep -q 'DIAMOND_JIT=' "$file" || continue
            stem="${file%.env}"
            for suffix in "$stem".*; do
                [ -e "$suffix" ] || continue
                rm -f "$suffix"
                removed=$((removed + 1))
            done
        done
    fi
    printf '%s' "$removed"
}

# Returns 0 when every target passed.
build_and_test() {
    local work="$1" jobs="$2" all_passed=0 target duration verdict last line
    section "Build and tests"
    say "  Each step is quiet while it runs; that's normal."
    while IFS='|' read -r target duration; do
        [ -z "$target" ] && continue
        say "  running make $target ($duration) ..."
        step_in "$work" 3600 make "-j$jobs" "${MAKE_ARGS[@]}" "$target"
        verdict="passed"
        [ "$STEP_STATUS" -ne 0 ] && verdict="FAILED" && all_passed=1
        last="$(grep -v '^[[:space:]]*$' "$STEP_OUTPUT" | tail -n 1)"
        say "    $verdict in ${STEP_SECONDS}s  ${last:0:80}"
        record "make $target: $verdict in ${STEP_SECONDS}s -- $last"
        if [ "$STEP_STATUS" -ne 0 ] && grep -qE 'No space left on device|Disk quota exceeded' "$STEP_OUTPUT"; then
            say "    (the disk filled up; this failure is not a Diamond problem)"
            record "NOTE: the machine ran out of disk space during this step, so the failure is an environment problem, not a Diamond one."
        fi
        if [ "$STEP_STATUS" -ne 0 ]; then
            record '```'
            tail_of >> "$REPORT"
            record '```'
        fi
        # Each test program leaves a multi-megabyte cache file behind; drop them before the
        # next step so the disk does not stay full.
        rm -f "$work"/tests/cases/*.d6c
    done <<EOF
$TEST_TARGETS
EOF
    return $all_passed
}

# step, run from inside directory $1.
step_in() {
    local dir="$1"
    shift
    local here="$PWD"
    cd "$dir" || return 1
    step "$@"
    cd "$here" || return 1
}

examples() {
    local work="$1" verdict
    section "Examples"
    step_in "$work" 900 bash examples/logstat/smoke_test.sh
    verdict="passed"
    [ "$STEP_STATUS" -ne 0 ] && verdict="FAILED"
    say "  logstat (diamond build): $verdict in ${STEP_SECONDS}s"
    record "logstat smoke test: $verdict in ${STEP_SECONDS}s"
    [ "$STEP_STATUS" -ne 0 ] && tail_of >> "$REPORT"
    local seconds
    step_in "$work/examples/chat" 300 "$work/build/facet" install
    seconds=$STEP_SECONDS
    if [ "$STEP_STATUS" -eq 0 ]; then
        step_in "$work/examples/chat" 300 "$work/build/diamond" smoke_test.d6
        seconds=$((seconds + STEP_SECONDS))
    fi
    verdict="passed"
    [ "$STEP_STATUS" -ne 0 ] && verdict="FAILED"
    say "  chat (registry install + WebSockets): $verdict in ${seconds}s"
    record "chat smoke test (facet install from cuts.dilang.tech): $verdict in ${seconds}s"
    [ "$STEP_STATUS" -ne 0 ] && tail_of >> "$REPORT"
}

# Seconds as a decimal ("1.234") from nanosecond-free tools: perl is on every Mac and most
# Linux boxes; fall back to whole seconds from $SECONDS-based timing.
now() { perl -MTime::HiRes=time -e 'printf "%.4f", time' 2> /dev/null || date +%s; }

benchmarks() {
    local work="$1" jobs="$2" entry name repeats started elapsed line
    section "Benchmarks (release build, interpreter)"
    say "  building an optimized copy (a few minutes) ..."
    step_in "$work" 3600 make "-j$jobs" "${MAKE_ARGS[@]}" release
    if [ "$STEP_STATUS" -ne 0 ]; then
        say "  release build FAILED; skipping benchmarks"
        record "release build FAILED"
        tail_of >> "$REPORT"
        return
    fi
    record "release build: ${STEP_SECONDS}s"
    local diamond="$work/build/diamond"
    export DIAMOND_NO_CACHE=1
    record "benchmark                   repeats  seconds/run"
    for entry in $BENCHMARKS; do
        name="${entry%%:*}"
        repeats="${entry##*:}"
        [ -e "$work/bench/$name.d6" ] || continue
        started="$(now)"
        if DIAMOND_REPEAT="$repeats" run_limited 600 "$diamond" "$work/bench/$name.d6" > /dev/null 2>&1; then
            elapsed="$(awk -v s="$started" -v e="$(now)" -v r="$repeats" 'BEGIN { printf "%10.4f", (e - s) / r }')"
        else
            elapsed="    FAILED"
        fi
        line="$(printf '%-27s %7s  %s' "$name" "$repeats" "$elapsed")"
        say "  $line"
        record "$line"
    done

    say
    say "  thread scaling (same work per thread; flat time = perfect scaling)"
    say "  (up to a minute per line)"
    record
    record "thread scaling: each thread does the same work; ideal is flat time"
    record "threads   seconds   work/second relative to 1 thread"
    local cores count baseline="" rate seconds script
    cores="$(cpu_count)"
    script="$REPO/tools/platform_report/thread_scaling.d6"
    for count in 1 2 4 8 16 32 "$cores"; do
        [ "$count" -le "$cores" ] && [ "$count" -le 32 ] && echo "$count"
    done | sort -un > "$STEP_OUTPUT.counts"
    while read -r count; do
        started="$(now)"
        if run_limited 1200 "$diamond" "$script" "$count" 20000000 > /dev/null 2>&1; then
            seconds="$(awk -v s="$started" -v e="$(now)" 'BEGIN { printf "%.2f", e - s }')"
            rate="$(awk -v c="$count" -v s="$seconds" 'BEGIN { printf "%.6f", c / s }')"
            [ -z "$baseline" ] && baseline="$rate"
            line="$(awk -v c="$count" -v s="$seconds" -v r="$rate" -v b="$baseline" \
                'BEGIN { printf "%7d   %7.2f   %6.2fx", c, s, r / b }')"
        else
            line="$(printf '%7d   FAILED' "$count")"
        fi
        say "    $line"
        record "$line"
    done < "$STEP_OUTPUT.counts"
    rm -f "$STEP_OUTPUT.counts"
    unset DIAMOND_NO_CACHE
}

# Runs a command with a time limit, output to the terminal's own streams.
run_limited() {
    local limit="$1"
    shift
    if [ -n "$TIMEOUT_CMD" ]; then "$TIMEOUT_CMD" "$limit" "$@"; else "$@"; fi
}

# Test cases that use threads, channels or supervisors and have a plain stdout
# expectation (the shape tests/stress_threads.sh re-runs on Linux).
threaded_cases() {
    local source stem
    for source in "$1"/tests/cases/*.d6; do
        stem="${source%.d6}"
        grep -qE 'Thread\.new|Channel\.|Supervisor\.new' "$source" || continue
        [ -e "$stem.expected" ] && [ ! -e "$stem.flags" ] && echo "$source"
    done
}

# The Linux stress loop (tests/stress_threads.sh) pins CPUs with taskset, which macOS
# lacks. Oversubscribe the machine with busy processes instead, so threads interleave
# unevenly. Apple Silicon's weak memory model is the reason this is worth running here.
stress_threads() {
    local work="$1" runs=20 cores diamond="$1/build/diamond" started source stem attempt
    local expected output status count=0 failed=0 verdict i failures="" line
    section "Threaded stress (Apple Silicon memory ordering)"
    cores="$(cpu_count)"
    local cases
    cases="$(threaded_cases "$work")"
    for source in $cases; do count=$((count + 1)); done
    i=0
    while [ "$i" -lt "$cores" ]; do
        yes > /dev/null &
        HOGS="$HOGS $!"
        i=$((i + 1))
    done
    started=$SECONDS
    say "  $count threaded test cases, $runs runs each, with $cores busy background processes (a few minutes) ..."
    for source in $cases; do
        stem="${source%.d6}"
        expected="$(cat "$stem.expected")"
        attempt=0
        while [ "$attempt" -lt "$runs" ]; do
            attempt=$((attempt + 1))
            output="$(
                cd "$work" || exit 1
                export DIAMOND_NO_CACHE=1
                if [ -e "$stem.env" ]; then
                    while IFS= read -r line; do
                        case "$line" in *=*) export "${line%%=*}=${line#*=}" ;; esac
                    done < "$stem.env"
                fi
                run_limited 90 "$diamond" "$source" 2>&1
            )"
            status=$?
            if [ "$status" -ne 0 ] || [ "$output" != "$expected" ]; then
                failed=$((failed + 1))
                failures="$failures
  $(basename "$stem"): failed on run $attempt (exit $status)
$(printf '%s\n' "$output" | tail -n 6)"
                break
            fi
        done
    done
    for i in $HOGS; do kill "$i" 2> /dev/null; done
    HOGS=""
    verdict="passed"
    [ "$failed" -ne 0 ] && verdict="FAILED ($failed case(s))"
    say "  $count cases x $runs runs: $verdict in $((SECONDS - started))s"
    record "threaded stress: $count cases x $runs runs under CPU oversubscription: $verdict in $((SECONDS - started))s"
    [ -n "$failures" ] && record "${failures#
}"
}

# The JIT is x86-64 only. On Apple Silicon asking for it must be a quiet no-op that still
# prints the right answer, never a crash.
jit_probe() {
    local work="$1" diamond="$1/build/diamond" program="$1/bench/dispatch_monomorphic.d6"
    local plain_status plain_output status output trace verdict same=0
    section "JIT request on this CPU"
    plain_output="$(DIAMOND_NO_CACHE=1 run_limited 300 "$diamond" "$program" 2>&1)"
    plain_status=$?
    local started=$SECONDS
    output="$(DIAMOND_NO_CACHE=1 DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 \
        run_limited 300 "$diamond" "$program" 2>&1)"
    status=$?
    if [ "$status" -eq 0 ] && [ "$plain_status" -eq 0 ] &&
        [ "$(printf '%s\n' "$output" | grep -v '^jit:')" = "$plain_output" ]; then
        same=1
    fi
    trace="$(printf '%s\n' "$output" | grep -m 1 '^jit:')"
    [ -z "$trace" ] && trace="no trace line"
    verdict="ok"
    [ "$same" -eq 0 ] && verdict="DIFFERS from the plain run"
    say "  DIAMOND_JIT=1 on $(uname -m): $verdict; $trace"
    record "DIAMOND_JIT=1 on $(uname -m): output $verdict in $((SECONDS - started))s; $trace"
}

write_report() {
    local folder="$HOME"
    [ -d "$HOME/Desktop" ] && folder="$HOME/Desktop"
    local path="$folder/diamond-report-$(date '+%Y%m%d-%H%M').txt"
    cp "$REPORT" "$path"
    say
    say "Report written to $path"
    say "Please send that file back. Thanks!"
}

main() {
    local base work jobs tests_passed=1 summary
    say "Diamond platform report"
    say "This builds Diamond, runs its tests and some benchmarks, and writes a"
    say "report file to send back. It takes roughly 30-60 minutes. Your checkout is"
    say "not modified; the work happens in a temporary copy."
    record "# Diamond platform report"
    machine_info
    if ! prerequisites; then
        write_report
        return 1
    fi
    base="$(check_free_space)"
    if [ -z "$base" ]; then
        write_report
        return 1
    fi
    build_environment
    jobs="$(cpu_count)"
    WORK_PARENT="$(mktemp -d "$base/diamond-report-XXXXXX")"
    work="$WORK_PARENT/diamond"
    # --no-hardlinks: the temporary directory may be on another filesystem.
    step 0 git clone --quiet --no-hardlinks "$REPO" "$work"
    if [ "$STEP_STATUS" -ne 0 ]; then
        say "Could not make a temporary copy of the checkout:"
        tail_of "$STEP_OUTPUT" 5
        record "could not copy the checkout:"
        tail_of >> "$REPORT"
        write_report
        return 1
    fi
    record "excluded known-gap test files: $(exclude_known_gaps "$work")"
    build_and_test "$work" "$jobs"
    tests_passed=$?
    examples "$work"
    jit_probe "$work"
    stress_threads "$work"
    benchmarks "$work" "$jobs"
    section "Summary"
    summary="all test targets passed"
    [ "$tests_passed" -ne 0 ] && summary="some test targets FAILED (details above)"
    say "  $summary"
    record "$summary"
    write_report
    return 0
}

main "$@"
