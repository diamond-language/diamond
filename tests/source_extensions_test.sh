#!/usr/bin/env bash
set -euo pipefail
unset DIAMOND_NO_CACHE
diamond="$(realpath ./build/diamond)"
facet="$(realpath ./build/facet)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

# New imports win over a same-named legacy source. Explicit legacy imports
# and extensionless legacy-only imports remain usable during migration.
printf 'def chosen() = 42\n' > choice.d6
printf 'def chosen() = 99\n' > choice.di
printf 'require "choice"\nputs(chosen())\n' > app.d6
[[ "$("$diamond" app.d6)" == 42 ]]
[[ -f app.d6c && ! -f app.dic ]]
[[ "$(DIAMOND_TRACE_CACHE=1 "$diamond" app.d6 2>&1)" == *"cache: hit"* ]]
printf 'require "choice.di"\nputs(chosen())\n' > explicit.d6
[[ "$("$diamond" explicit.d6)" == 99 ]]
printf 'require "choice.d6"\nputs(chosen())\n' > explicit.d6
[[ "$("$diamond" explicit.d6)" == 42 ]]
printf 'def old_value() = 7\n' > old.di
printf 'require "old"\nputs(old_value())\n' > legacy.di
[[ "$("$diamond" legacy.di)" == 7 ]]
[[ -f legacy.dic ]]
printf 'puts(21 * 2)\n' > no_extension
[[ "$("$diamond" no_extension)" == 42 ]]
[[ -f no_extension.d6c ]]

# A cut can be checked, packed, verified and required with its new public
# entry point. Old releases remain usable; both cache suffixes stay out of
# archives, and the source audit still checks legacy files.
mkdir -p example/lib
printf '%s\n' '{"name":"example","version":"1.0.0","summary":"An example","license":"MIT","maintainers":[{"name":"Test","contact":"test@example.com"}]}' > example/diamond.cut
printf 'example\n' > example/README.md
printf 'MIT\n' > example/LICENSE
printf 'def cut_value() = 18\n' > example/lib/example.d6
for suffix in d6c dic; do
    printf 'discard\n' > "example/lib/ignore.$suffix"
    if "$facet" pack example "cache-$suffix.tar" > pack.out 2>&1; then
        echo "facet packed a .$suffix cache" >&2
        exit 1
    fi
    grep -q 'excluded or sensitive path' pack.out
    rm "example/lib/ignore.$suffix"
done
"$facet" check example >/dev/null
"$facet" pack example modern.tar >/dev/null
"$facet" verify modern.tar >/dev/null
tar -tf modern.tar > files
! grep -Eq '\.(d6c|dic)$' files
mkdir -p cuts/example
tar -xf modern.tar -C cuts/example
printf 'require_cut "example"\nputs(cut_value())\n' > cut_app.d6
[[ "$("$diamond" cut_app.d6)" == 18 ]]
mv example/lib/example.d6 example/lib/example.di
"$facet" pack example legacy.tar >/dev/null
"$facet" verify legacy.tar >/dev/null
printf 'require_cut "unlisted"\n' > example/lib/extra.di
if "$facet" pack example undeclared.tar > pack.out 2>&1; then
    echo 'facet skipped the legacy source import audit' >&2
    exit 1
fi
grep -q 'unlisted' pack.out
rm example/lib/extra.di
rm cuts/example/lib/example.d6
tar -xf legacy.tar -C cuts/example
[[ "$("$diamond" cut_app.d6)" == 18 ]]
printf 'def cut_value() = 20\n' > cuts/example/example.d6
[[ "$("$diamond" cut_app.d6)" == 20 ]]
printf 'def cut_value() = 24\n' > cuts/example/lib/example.d6
[[ "$("$diamond" cut_app.d6)" == 24 ]]

printf 'not a cache\n' > example/lib/ignore.d6c
printf 'not a cache\n' > example/lib/ignore.dic
"$diamond" cache clear . >/dev/null
[[ ! -f app.d6c && ! -f legacy.dic && ! -f no_extension.d6c ]]
[[ -f example/lib/ignore.d6c && -f example/lib/ignore.dic ]]
echo 'source extension tests passed'
