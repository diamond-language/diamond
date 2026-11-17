#!/usr/bin/env bash
# Builds a .deb or .rpm holding d6, facet, d6-lsp, d6-dap and the AOT kit `d6
# build` links against, installed under /usr, or a tarball with the same files
# under one relocatable directory (bin/, lib/diamond/aot).
#
#   bash packaging/build.sh deb|rpm|tar [OUTPUT_DIR]
#
# Run it on the distribution the package is for: Ubuntu (with dpkg-dev) for a
# .deb or the tarball, Fedora (with rpm-build) for a .rpm. packaging/ci.sh does
# all three in containers. The build is a clean one: it runs `make clean` first.
set -euo pipefail

format="${1:-}"
case "$format" in
    deb|rpm|tar) ;;
    *) echo "usage: packaging/build.sh deb|rpm|tar [OUTPUT_DIR]" >&2; exit 64 ;;
esac
repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"
version="$(sed -n 's/.*DIAMOND_VERSION\[\] = "\(.*\)";/\1/p' src/main.c)"
# A development version (0.27.0-dev) is packaged as 0.27.0~dev: RPM forbids '-' in
# Version, and '~' sorts it before the 0.27.0 release in both dpkg and rpm.
package_version="${version//-/\~}"
# File names keep the upstream spelling (1.0.0-rc1): GitHub rewrites a "~" in
# release asset names, which would break download links.
jobs="$(nproc 2>/dev/null || echo 4)"

# The Makefile defaults to -march=native, which ties a binary to the CPU that
# built it. A package runs elsewhere, so it targets x86-64-v3, the documented
# minimum (see the Makefile's CFLAGS_RELEASE comment). facet, diamond-lsp and
# diamond-dap build with CFLAGS_DEBUG, so the package overrides that too.
march=""
[[ "$(uname -m)" == x86_64 ]] && march="-march=x86-64-v3"
# A package reports exactly the version it is packaged as, never a git revision
# (tools/version_suffix.sh).
export D6_VERSION_SUFFIX=
make clean >/dev/null
out="$(mkdir -p "${2:-build/packages}" && cd "${2:-build/packages}" && pwd)"
# Two runs: `release` drives its own sub-make, which races with the tool
# targets for shared generated files when both run in one parallel make.
tools_flags="-O2 -g -DNDEBUG $march"
# The tarball runs on distributions whose libcrypt sonames differ (libcrypt.so.1 on Debian
# and Ubuntu, .so.2 on Fedora), so its programs link libxcrypt statically. The kit `make
# install` copies still says -lcrypt: `d6 build` links against the machine it runs on.
# OpenSSL is static too: Fedora 45 and Arch have moved to OpenSSL 4 (libssl.so.4) and
# have no libssl.so.3. --exclude-libs keeps its symbols out of the dynamic symbol table:
# libpq and libmariadb on the host load the host's own OpenSSL, and an exported SSL_*
# definition in d6 would capture their calls. Ubuntu's libcrypto.a also needs libjitterentropy
# and libzstd (`pkg-config --static --libs libcrypto`), which are linked statically with it.
openssl_archives=(libssl.a libcrypto.a libjitterentropy.a libzstd.a)
exclude_libs="-Wl,--exclude-libs,$(IFS=:; echo "${openssl_archives[*]}")"
static_openssl="$exclude_libs ${openssl_archives[*]/#/-l:}"
link_overrides=()
[[ "$format" == tar ]] && link_overrides=(LDLIBS_CRYPT=-l:libcrypt.a "OPENSSL_LIBS=$static_openssl")
make -j"$jobs" CC=gcc CFLAGS_RELEASE="-O3 -DNDEBUG $march" CFLAGS_DEBUG="$tools_flags" "${link_overrides[@]}" release
make -j"$jobs" CC=gcc CFLAGS_DEBUG="$tools_flags" "${link_overrides[@]}" facet lsp dap
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

if [[ "$format" == tar ]]; then
    # An install prefix (bin/, lib/diamond/aot) under one top-level directory, for
    # `mise use github:diamond-language/d6` or unpacking anywhere: d6 finds its kit relative
    # to its own executable.
    root="d6-${version}-linux-$(uname -m)"
    mkdir "$work/$root"
    cp -a "$stage/usr/." "$work/$root/"
    cp LICENSE README.md "$work/$root/"
    # `d6 build` links programs against the same static OpenSSL: the archives go in the kit,
    # and link.args names them relative to it ({kit}/, expanded by d6), so building needs
    # no OpenSSL development package and works on OpenSSL 4 systems.
    kit="$work/$root/lib/diamond/aot"
    kit_archives=""
    for archive in "${openssl_archives[@]}"; do
        cp "$(gcc -print-file-name="$archive")" "$kit/"
        kit_archives+="\\n{kit}/$archive"
    done
    # -lssl and -lcrypto are adjacent lines; the first becomes the whole static set.
    sed -i -e "s|^-lssl\$|$exclude_libs$kit_archives|" -e '/^-lcrypto$/d' "$kit/link.args"
    grep -qx '{kit}/libcrypto.a' "$kit/link.args" && ! grep -qxE -- '-l(ssl|crypto)' "$kit/link.args" || {
        echo "packaging: the kit's link.args does not name the static OpenSSL archives" >&2
        exit 1
    }
    mapfile -t binaries < <(find "$work/$root/bin" -type f)
    strip --strip-unneeded "${binaries[@]}"
    for binary in "${binaries[@]}"; do
        if readelf -d "$binary" | grep -q "NEEDED.*\[libcrypt\.so"; then
            echo "packaging: $binary needs a shared libcrypt; the tarball links it statically" >&2
            exit 1
        fi
        if readelf -d "$binary" | grep -qE "NEEDED.*\[lib(ssl|crypto)\.so"; then
            echo "packaging: $binary needs a shared OpenSSL; the tarball links it statically" >&2
            exit 1
        fi
        if nm -D --defined-only "$binary" | grep -qE " (SSL|EVP|OPENSSL|CRYPTO|ZSTD|jent)_"; then
            echo "packaging: $binary exports OpenSSL symbols" >&2
            exit 1
        fi
    done
    tar -C "$work" --owner=0 --group=0 -czf "$out/$root.tar.gz" "$root"
    echo "$out/$root.tar.gz"
elif [[ "$format" == deb ]]; then
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
        printf 'Version: %s-1\n' "$package_version"
        printf 'Architecture: %s\n' "$arch"
        printf 'Maintainer: d6 maintainers <https://github.com/diamond-language/d6/issues>\n'
        printf 'Installed-Size: %s\n' "$(du -sk "$stage/usr" | cut -f1)"
        # facet downloads cuts with curl and clones git sources with git.
        printf 'Depends: %s, curl, ca-certificates\n' "$depends"
        printf 'Recommends: git, gcc, libsqlite3-dev, libpq-dev, libmariadb-dev, libssl-dev, libcrypt-dev, zlib1g-dev\n'
        printf 'Section: devel\n'
        printf 'Priority: optional\n'
        printf 'Homepage: https://rolld6.codes\n'
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
        --define "_build_name_fmt %%{NAME}-${version}-%%{RELEASE}.%%{ARCH}.rpm" \
        --define "diamond_version $package_version" \
        --define "diamond_stage $stage" >/dev/null
    ls "$out"/d6-lang-"$version"-*.rpm
fi
