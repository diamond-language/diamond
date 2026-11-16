#!/bin/sh
# Checks a release tarball (packaging/build.sh tar) inside a clean distribution container
# that has only the runtime libraries, a C compiler and the development headers `d6 build`
# links against. Run by packaging/ci.sh with the tarball in /packages:
#
#   sh /packaging/tarball_smoke.sh VERSION
#
# It unpacks to a path nothing was built at (d6 must find its kit relative to itself), then
# runs packaging/portable_tools_smoke.sh (d6, and a facet project installed from the
# registry) and builds and runs a standalone program.
set -eu

version="${1:?usage: tarball_smoke.sh VERSION}"
prefix="/opt/somewhere-else"
mkdir -p "$prefix"
tar -xzf /packages/d6-"$version"-linux-*.tar.gz -C "$prefix" --strip-components=1
PATH="$prefix/bin:$PATH"
export PATH

[ "$(d6 --version)" = "d6 $version" ]
echo "ok d6 --version"
for tool in facet d6-lsp d6-dap diamond; do
    command -v "$tool" > /dev/null
done
echo "ok tools on PATH"

sh /packaging/portable_tools_smoke.sh

work="$(mktemp -d)"
cd "$work"
printf '%s\n' 'puts("built #{ARGV.join(",")}")' > hello.d6
d6 build hello.d6 > /dev/null
[ "$(./hello a b)" = "built a,b" ]
echo "ok d6 build"
