#!/usr/bin/env bash
# Checks an installed diamond-lang package: the tools are on PATH under /usr,
# a script and -e run, a facet new project's tests pass, and diamond build
# links that project into an executable with the packaged kit. Runs in a clean
# container after the package is installed (see packaging/ci.sh).
set -euo pipefail

expected_version="${1:?usage: smoke_test.sh VERSION}"
for tool in diamond facet diamond-lsp diamond-dap; do
    [[ "$(readlink -f "$(command -v "$tool")")" == "/usr/bin/$tool" ]] || { echo "$tool is not /usr/bin/$tool" >&2; exit 1; }
done
[[ "$(diamond --version)" == "diamond $expected_version" ]]
[[ "$(diamond -e '20 + 22')" == 42 ]]

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
printf 'puts("Hello, #{ARGV[0]}!")\n' > hello.d6
[[ "$(diamond hello.d6 Ada)" == "Hello, Ada!" ]]

facet new cli greeter >/dev/null
cd greeter
bash test.sh >/dev/null
[[ "$(diamond main.d6 Ada)" == "Hello, Ada!" ]]
diamond build main.d6 -o greeter >/dev/null
[[ "$(./greeter Grace)" == "Hello, Grace!" ]]
echo "package smoke test passed"
