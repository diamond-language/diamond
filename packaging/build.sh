#!/usr/bin/env bash
# Builds a .deb or .rpm holding diamond, facet, diamond-lsp, diamond-dap and the
# AOT kit `diamond build` links against, installed under /usr.
#
#   bash packaging/build.sh deb|rpm [OUTPUT_DIR]
#
# Run it on the distribution the package is for: Ubuntu (with dpkg-dev) for a
# .deb, Fedora (with rpm-build) for a .rpm. packaging/ci.sh does both in
# containers. The build is a clean one: it runs `make clean` first.
set -euo pipefail

format="${1:-}"
case "$format" in
    deb|rpm) ;;
    *) echo "usage: packaging/build.sh deb|rpm [OUTPUT_DIR]" >&2; exit 64 ;;
esac
repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"
version="$(sed -n 's/.*DIAMOND_VERSION\[\] = "\(.*\)";/\1/p' src/main.c)"
jobs="$(nproc 2>/dev/null || echo 4)"

# The Makefile defaults to -march=native, which ties a binary to the CPU that
# built it. A package runs elsewhere, so it targets x86-64-v3, the documented
# minimum (see the Makefile's CFLAGS_RELEASE comment). facet, diamond-lsp and
# diamond-dap build with CFLAGS_DEBUG, so the package overrides that too.
march=""
[[ "$(uname -m)" == x86_64 ]] && march="-march=x86-64-v3"
make clean >/dev/null
out="$(mkdir -p "${2:-build/packages}" && cd "${2:-build/packages}" && pwd)"
# Two runs: `release` drives its own sub-make, which races with the tool
# targets for shared generated files when both run in one parallel make.
tools_flags="-O2 -g -DNDEBUG $march"
make -j"$jobs" CC=gcc CFLAGS_RELEASE="-O3 -DNDEBUG $march" CFLAGS_DEBUG="$tools_flags" release
make -j"$jobs" CC=gcc CFLAGS_DEBUG="$tools_flags" facet lsp dap
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
stage="$work/stage"
make CC=gcc CFLAGS_DEBUG="$tools_flags" install DESTDIR="$stage" PREFIX=/usr >/dev/null

# The binaries must not report themselves as debug builds.
version_line="$("$stage/usr/bin/diamond" --version)"
if [[ "$version_line" != "diamond $version" ]]; then
    echo "packaging: unexpected version line: $version_line" >&2
    exit 1
fi

description="Diamond is a gradually typed, object-oriented language with a register-based
bytecode VM. This package provides the diamond interpreter and REPL, the facet
package manager, the diamond-lsp language server, the diamond-dap debug adapter,
and the runtime kit diamond build links standalone executables against."

if [[ "$format" == deb ]]; then
    doc="$stage/usr/share/doc/diamond-lang"
    mkdir -p "$doc"
    cp LICENSE "$doc/copyright"
    cp README.md "$doc/README.md"
    strip --strip-unneeded "$stage"/usr/bin/*
    arch="$(dpkg --print-architecture)"
    # dpkg-shlibdeps derives the runtime library packages from the binaries'
    # NEEDED entries; it insists on a debian/control in the working directory.
    mkdir -p "$work/shlibs/debian"
    printf 'Source: diamond-lang\n\nPackage: diamond-lang\nArchitecture: any\n' > "$work/shlibs/debian/control"
    depends="$(cd "$work/shlibs" && dpkg-shlibdeps -O "$stage"/usr/bin/* | sed -n 's/^shlibs:Depends=//p')"
    mkdir -p "$stage/DEBIAN"
    {
        printf 'Package: diamond-lang\n'
        printf 'Version: %s-1\n' "$version"
        printf 'Architecture: %s\n' "$arch"
        printf 'Maintainer: Diamond maintainers <https://github.com/diamond-language/diamond/issues>\n'
        printf 'Installed-Size: %s\n' "$(du -sk "$stage/usr" | cut -f1)"
        # facet downloads cuts with curl and clones git sources with git.
        printf 'Depends: %s, curl, ca-certificates\n' "$depends"
        printf 'Recommends: git, gcc, libsqlite3-dev, libpq-dev, libmariadb-dev, libssl-dev, libcrypt-dev, zlib1g-dev\n'
        printf 'Section: devel\n'
        printf 'Priority: optional\n'
        printf 'Homepage: https://getdiamond.codes\n'
        printf 'Description: The Diamond programming language\n'
        sed 's/^/ /' <<<"$description"
    } > "$stage/DEBIAN/control"
    dpkg-deb --root-owner-group --build "$stage" "$out/diamond-lang_${version}-1_${arch}.deb" >/dev/null
    echo "$out/diamond-lang_${version}-1_${arch}.deb"
else
    mkdir -p "$stage/usr/share/licenses/diamond-lang" "$stage/usr/share/doc/diamond-lang"
    cp LICENSE "$stage/usr/share/licenses/diamond-lang/LICENSE"
    cp README.md "$stage/usr/share/doc/diamond-lang/README.md"
    rpmbuild -bb packaging/diamond-lang.spec \
        --define "_topdir $work/rpm" \
        --define "_rpmdir $out" \
        --define "_build_name_fmt %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" \
        --define "diamond_version $version" \
        --define "diamond_stage $stage" >/dev/null
    ls "$out"/diamond-lang-"$version"-*.rpm
fi
