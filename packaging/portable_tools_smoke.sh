#!/bin/sh
# Checks the portable tools (tools/build_portable.sh) inside a clean
# distribution container: portable_smoke.d6 exercises d6 itself, then a
# facet new project installs its test cut from the registry and its tests
# pass. Run by tools/test_portable_distros.sh with the tools on PATH, so it
# is POSIX sh: not every image has bash.
set -eu

d6 /smoke.d6
d6 -e '20 + 22' | grep -qx 42
echo "ok d6 -e"

[ "${PORTABLE_SMOKE_OFFLINE:-}" = 1 ] && exit 0
work="$(mktemp -d)"
cd "$work"
facet new cli greeter >/dev/null
cd greeter
facet install >/dev/null
bash test.sh >/dev/null
[ "$(d6 main.d6 Ada)" = "Hello, Ada!" ]
echo "ok facet"
