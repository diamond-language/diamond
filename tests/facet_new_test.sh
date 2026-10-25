#!/usr/bin/env bash
# `facet new`: every kind generates a project whose own tests pass and whose
# entry point runs; bad kinds, names and paths are refused; the reserved-name
# list and the web template's dependency versions match the sources they copy.
set -euo pipefail
cd "$(dirname "$0")/.."
root="$PWD"
facet="${FACET_BIN:-$root/build/facet}"
diamond="${DIAMOND_BIN:-$root/build/diamond}"
export DIAMOND_BIN="$diamond"
export DIAMOND_NO_CACHE=1
work="$(mktemp -d)"
server_pid=""
cleanup() {
    [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true
    rm -rf "$work"
}
trap cleanup EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass=0
ok() { pass=$((pass + 1)); }

client() { "$diamond" "$root/tests/support/facet_new_client.d6" "$@"; }
free_port() { client free-port; }

wait_for_tcp() {
    for _ in $(seq 1 100); do
        if (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null; then return 0; fi
        sleep 0.1
    done
    return 1
}

stop_server() {
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    server_pid=""
}

# Without a usable git identity the lib template falls back to placeholders.
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null HOME="$work"

cd "$work"

# --- every kind generates a project whose tests pass ---
for kind in cli web tcp udp lib; do
    "$facet" new "$kind" "demo_$kind" >/dev/null || fail "facet new $kind"
    [[ -f "demo_$kind/lib/demo_$kind.d6" && -f "demo_$kind/test/demo_${kind}_test.d6" ]] ||
        fail "$kind: missing lib or test file"
    cmp -s "demo_$kind/test/minitest.d6" "$root/lib/minitest.d6" || fail "$kind: minitest copy differs"
    [[ -x "demo_$kind/test.sh" ]] || fail "$kind: test.sh is not executable"
    grep -q '{{' -r "demo_$kind" && fail "$kind: unsubstituted placeholder"
    if [[ "$kind" == web ]]; then
        bash "$root/tools/install_local_cuts.sh" demo_web >/dev/null
    fi
    output="$(bash "demo_$kind/test.sh" 2>&1)" || fail "$kind: generated tests failed: $output"
    [[ "$output" == *" 0 failed, 0 errors"* ]] || fail "$kind: unexpected test output: $output"
    ok
done

# --- the entry points run ---
[[ "$("$diamond" demo_cli/main.d6 Ada)" == "Hello, Ada!" ]] || fail "cli greeting"
set +e
"$diamond" demo_cli/main.d6 --bogus 2>/dev/null
status=$?
set -e
[[ "$status" == 64 ]] || fail "cli usage error exited $status"
ok

port="$(free_port)"
(builtin cd demo_web && PORT="$port" exec "$diamond" main.d6 >/dev/null 2>&1) &
server_pid=$!
wait_for_tcp "$port" || fail "web server did not start"
[[ "$(curl -s --max-time 5 "http://127.0.0.1:$port/")" == "Hello from demo_web!" ]] || fail "web GET /"
[[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "http://127.0.0.1:$port/nope")" == 404 ]] ||
    fail "web 404"
stop_server
ok

port="$(free_port)"
(PORT="$port" exec "$diamond" demo_tcp/main.d6 >/dev/null 2>&1) &
server_pid=$!
wait_for_tcp "$port" || fail "tcp server did not start"
reply="$(client tcp "$port")"
[[ "$reply" == $'echo: hello
PONG' ]] || fail "tcp replies: $reply"
stop_server
ok

port="$(free_port)"
(PORT="$port" exec "$diamond" demo_udp/main.d6 >/dev/null 2>&1) &
server_pid=$!
reply="$(client udp "$port")"
[[ "$reply" == PONG ]] || fail "udp reply: $reply"
stop_server
ok

# --- the lib project is publishable, with placeholder maintainer ---
"$facet" check demo_lib >/dev/null || fail "facet check on the lib project"
grep -q '"name": "Your Name", "contact": "you@example.com"' demo_lib/diamond.cut ||
    fail "lib placeholder maintainer"
grep -q "Copyright (c) $(date -u +%Y) Your Name" demo_lib/LICENSE || fail "lib LICENSE"
ok

# --- the git identity fills the maintainer when it is usable ---
printf '[user]\n\tname = Ada Lovelace\n\temail = ada@example.com\n' >"$work/gitconfig"
GIT_CONFIG_GLOBAL="$work/gitconfig" "$facet" new lib ada_lib >/dev/null
grep -q '"name": "Ada Lovelace", "contact": "ada@example.com"' ada_lib/diamond.cut ||
    fail "lib maintainer from git config"
# git strips bare quotes from config values; escaped ones survive.
printf '[user]\n\tname = Ada \\"quoted\\"\n\temail = ada@example.com\n' >"$work/gitconfig"
GIT_CONFIG_GLOBAL="$work/gitconfig" "$facet" new lib quoted_lib >/dev/null
grep -q '"name": "Your Name", "contact": "ada@example.com"' quoted_lib/diamond.cut ||
    fail "a quote in user.name must fall back to the placeholder"
ok

# --- refusals ---
refuses() {
    local expected="$1" message="$2"
    shift 2
    set +e
    output="$("$facet" "$@" 2>&1)"
    status=$?
    set -e
    [[ "$status" == "$expected" && "$output" == *"$message"* ]] ||
        fail "facet $*: expected $expected/'$message', got $status/'$output'"
}
refuses 64 "usage: facet new" new
refuses 64 "usage: facet new" new cli
refuses 64 "unknown project kind 'rails'" new rails app
refuses 64 "not a valid project name" new cli MyApp
refuses 64 "not a valid project name" new cli 1app
refuses 64 "not a valid project name" new cli my-app
refuses 64 "not a valid project name" new cli facet
refuses 65 "already exists" new cli demo_cli
refuses 73 "cannot create" new cli missing_parent/app
refuses 64 "module Rack" new web rack
refuses 64 "module SecurityHeaders" new web security_headers
[[ ! -e app && ! -e missing_parent ]] || fail "a refused command left files behind"
"$facet" new web trailing_slash/ >/dev/null && [[ -f trailing_slash/lib/trailing_slash.d6 ]] ||
    fail "a trailing slash on the path"
ok

# --- the reserved list covers every name a project module could shadow ---
# CamelCase -> snake_case, the inverse of facet's camel_case for these names.
snake() { sed -E 's/([a-z0-9])([A-Z])/\1_\2/g; s/([A-Z]+)([A-Z][a-z])/\1_\2/g' <<<"$1" | tr '[:upper:]' '[:lower:]'; }
camel() { sed -E 's/(^|_)([a-z0-9])/\U\2/g' <<<"$1"; }
check_reserved() {
    local kind="$1" name="$2" lower
    lower="$(snake "$name")"
    # Only a name facet could produce from a project name can collide.
    [[ "$(camel "$lower")" == "$name" ]] || return 0
    refuses 64 "module $name" new "$kind" "$lower"
}
names="$(sed -n '/^builtin_namespace_calls\[\]=/,/^};/p' "$root/src/compiler_names.c" | grep -oE '"[A-Z][A-Za-z0-9]*"' | tr -d '"')"
names+=" $(sed -n '/{"Int",DIAMOND_TYPE_INT}/,/};/p' "$root/src/compiler.c" | grep -oE '"[A-Z][A-Za-z0-9]*"' | tr -d '"')"
names+=" $(grep -oE '\]=\{"[A-Z][A-Za-z0-9]*"' "$root/src/compiler_program.c" | grep -oE '[A-Z][A-Za-z0-9]*')"
names+=" $(grep -hoE '^\s*(class|module|struct) [A-Z][A-Za-z0-9]*' "$root"/lib/core.d6 "$root"/lib/core/*.d6 "$root/lib/minitest.d6" | awk '{print $2}')"
count=0
for name in $names; do
    for kind in cli web tcp udp lib; do check_reserved "$kind" "$name"; done
    count=$((count + 1))
done
[[ "$count" -gt 50 ]] || fail "reserved-name extraction found only $count names"

# A web project also cannot take its dependencies' names or their classes.
web_deps="gremlin rack http logger"
for dep in $web_deps; do
    refuses 64 "already uses" new web "$dep"
    # Every cut the template pulls in must be in this list.
    for transitive in $(client deps "$root/packages/$dep/diamond.cut"); do
        [[ " $web_deps " == *" $transitive "* ]] || fail "web dependency $dep needs $transitive, which is not reserved"
    done
    for name in $(grep -rhoE '^\s*(class|module|struct) [A-Z][A-Za-z0-9]*' "$root/packages/$dep/lib" | awk '{print $2}' | sort -u); do
        check_reserved web "$name"
    done
done
ok

# --- the web template's constraints accept the versions in packages/ ---
# Caret on 0.x pins the minor version; on 1.0 and later, the major.
while read -r name wanted; do
    have="$(client version "$root/packages/$name/diamond.cut")"
    if [[ "${wanted%%.*}" == 0 ]]; then line="${wanted%.*}"; else line="${wanted%%.*}"; fi
    [[ "$have" == "$line".* ]] || fail "web template wants $name ^$wanted, packages/ has $have"
    [[ "$(printf '%s\n%s\n' "$wanted" "$have" | sort -V | head -1)" == "$wanted" ]] ||
        fail "web template wants $name ^$wanted, packages/ has older $have"
done < <(sed -nE 's/^ *"([a-z_0-9]+)": \{"registry": "[^"]*", "version": "\^([0-9.]+)"\}.*/\1 \2/p' \
    "$root/tools/new_templates/web/diamond.cut")
[[ "$(grep -c '"version": "^' "$root/tools/new_templates/web/diamond.cut")" == 2 ]] ||
    fail "web template dependency lines did not parse"
ok

echo "facet new tests passed ($pass checks)"
