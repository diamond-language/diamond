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
version_line="$("$stage/usr/bin/d6" --version)"
if [[ "$version_line" != "d6 $version" ]]; then
    echo "packaging: unexpected version line: $version_line" >&2
    exit 1
fi

description="d6 is a gradually typed, object-oriented language with a register-based
bytecode VM. This package provides the d6 interpreter and REPL, the facet
package manager, the d6-lsp language server, the d6-dap debug adapter, and the
runtime kit d6 build links standalone executables against. The former diamond,
diamond-lsp and diamond-dap commands remain as links to them."

if [[ "$format" == deb ]]; then
    doc="$stage/usr/share/doc/d6-lang"
    mkdir -p "$doc"
    cp LICENSE "$doc/copyright"
    cp README.md "$doc/README.md"
    # The former command names are symlinks; only the real binaries get processed.
    mapfile -t binaries < <(find "$stage/usr/bin" -type f)
    strip --strip-unneeded "${binaries[@]}"
    arch="$(dpkg --print-architecture)"
    # dpkg-shlibdeps derives the runtime library packages from the binaries'
    # NEEDED entries; it insists on a debian/control in the working directory.
    mkdir -p "$work/shlibs/debian"
    printf 'Source: d6-lang\n\nPackage: d6-lang\nArchitecture: any\n' > "$work/shlibs/debian/control"
    depends="$(cd "$work/shlibs" && dpkg-shlibdeps -O "${binaries[@]}" | sed -n 's/^shlibs:Depends=//p')"
    mkdir -p "$stage/DEBIAN"
    {
        printf 'Package: d6-lang\n'
        # The package was called diamond-lang until the rename; this one replaces it.
        printf 'Provides: diamond-lang\n'
        printf 'Replaces: diamond-lang\n'
        printf 'Conflicts: diamond-lang\n'
        printf 'Version: %s-1\n' "$version"
        printf 'Architecture: %s\n' "$arch"
        printf 'Maintainer: d6 maintainers <https://github.com/diamond-language/diamond/issues>\n'
        printf 'Installed-Size: %s\n' "$(du -sk "$stage/usr" | cut -f1)"
        # facet downloads cuts with curl and clones git sources with git.
        printf 'Depends: %s, curl, ca-certificates\n' "$depends"
        printf 'Recommends: git, gcc, libsqlite3-dev, libpq-dev, libmariadb-dev, libssl-dev, libcrypt-dev, zlib1g-dev\n'
        printf 'Section: devel\n'
        printf 'Priority: optional\n'
        printf 'Homepage: https://getdiamond.codes\n'
        printf 'Description: The d6 programming language\n'
        sed 's/^/ /' <<<"$description"
    } > "$stage/DEBIAN/control"
    dpkg-deb --root-owner-group --build "$stage" "$out/d6-lang_${version}-1_${arch}.deb" >/dev/null
    echo "$out/d6-lang_${version}-1_${arch}.deb"
else
    mkdir -p "$stage/usr/share/licenses/d6-lang" "$stage/usr/share/doc/d6-lang"
    cp LICENSE "$stage/usr/share/licenses/d6-lang/LICENSE"
    cp README.md "$stage/usr/share/doc/d6-lang/README.md"
    rpmbuild -bb packaging/d6-lang.spec \
        --define "_topdir $work/rpm" \
        --define "_rpmdir $out" \
        --define "_build_name_fmt %%{NAME}-%%{VERSION}-%%{RELEASE}.%%{ARCH}.rpm" \
        --define "diamond_version $version" \
        --define "diamond_stage $stage" >/dev/null
    ls "$out"/d6-lang-"$version"-*.rpm
fi
