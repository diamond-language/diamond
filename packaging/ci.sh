#!/usr/bin/env bash
# Builds a package in one container and installs it into a fresh container of
# the same image, then runs packaging/smoke_test.sh there. The second container
# starts with none of the build dependencies, so the package's own Depends or
# Requires, plus its Recommends (which container images skip unless asked;
# desktop and server installs take them by default), must bring in everything. The
# image is upgraded first: neither apt nor dnf upgrades an installed package
# (glibc for gcc, libssl3t64 for libssl-dev) just to satisfy a Recommends, and
# an image lags behind its updates.
#
#   bash packaging/ci.sh deb|rpm [IMAGE]
#
# Defaults: ubuntu:26.04 for deb, fedora:44 for rpm. CONTAINER selects the
# runtime (default docker; podman works). Packages land in build/packages/FORMAT
# (one directory each: on a CI runner the container writes them as root, so a
# later run in the same job could not clear the other format's files).
set -euo pipefail

format="${1:-}"
case "$format" in
    deb) image="${2:-ubuntu:26.04}" ;;
    rpm) image="${2:-fedora:44}" ;;
    *) echo "usage: packaging/ci.sh deb|rpm [IMAGE]" >&2; exit 64 ;;
esac
runtime="${CONTAINER:-docker}"
repo="$(cd "$(dirname "$0")/.." && pwd)"
version="$(sed -n 's/.*DIAMOND_VERSION\[\] = "\(.*\)";/\1/p' "$repo/src/main.c")"

if [[ "$format" == deb ]]; then
    build_deps='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y gcc make git dpkg-dev libssl-dev libsqlite3-dev libpq-dev zlib1g-dev libmariadb-dev libcrypt-dev'
    install_package='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get upgrade -y && DEBIAN_FRONTEND=noninteractive apt-get install -y --install-recommends /packages/d6-lang_*.deb'
else
    build_deps='dnf install -y gcc make git rpm-build openssl-devel sqlite-devel libpq-devel mariadb-connector-c-devel libxcrypt-devel zlib-devel'
    install_package='dnf upgrade -y && dnf install -y --setopt=install_weak_deps=True /packages/d6-lang-*.rpm'
fi

out="$repo/build/packages/$format"
rm -rf "$out"
mkdir -p "$out"
# Build from a copy so the clean build cannot touch the checkout's own build
# outputs (build/, reginold's objects).
"$runtime" run --rm --security-opt label=disable -v "$repo:/src:ro" -v "$out:/out" "$image" \
    bash -c "set -e; $build_deps >/dev/null; mkdir /work
        tar -C /src --exclude=./build --exclude='./reginold/*.o' --exclude='./reginold/*.a' -cf - . | tar -C /work -xf -
        bash /work/packaging/build.sh $format /out"
"$runtime" run --rm --security-opt label=disable -v "$out:/packages:ro" -v "$repo/packaging:/packaging:ro" "$image" \
    bash -c "set -e; $install_package >/dev/null; bash /packaging/smoke_test.sh $version"

# Until 0.22.0 the package was called diamond-lang. Installing d6-lang over the
# last release under that name must replace it, not sit beside it. Only the
# default images on x86-64 match those release files: 0.22.0 had no arm64 build.
# The release file is fetched here, with retries and an API fallback
# (tools/fetch_release_asset.sh), and mounted read-only at /former.
former="$repo/build/packages/former"
if [[ "$(uname -m)" != x86_64 ]]; then
    exit 0
elif [[ "$format" == deb && "$image" == ubuntu:26.04 ]]; then
    former_file=diamond-lang_0.22.0-1_amd64.deb
    install_former="apt-get update && cp /former/$former_file /tmp/former.deb && DEBIAN_FRONTEND=noninteractive apt-get upgrade -y && DEBIAN_FRONTEND=noninteractive apt-get install -y --install-recommends /tmp/former.deb"
    former_gone='! dpkg-query -W -f="\${Status}" diamond-lang 2>/dev/null | grep -q "ok installed"'
elif [[ "$format" == rpm && "$image" == fedora:44 ]]; then
    former_file=diamond-lang-0.22.0-1.fc44.x86_64.rpm
    install_former="dnf upgrade -y && dnf install -y --setopt=install_weak_deps=True /former/$former_file"
    former_gone='! rpm -q diamond-lang'
else
    exit 0
fi
mkdir -p "$former"
sh "$repo/tools/fetch_release_asset.sh" diamond-language/diamond v0.22.0 "$former_file" "$former/$former_file"
"$runtime" run --rm --security-opt label=disable -v "$out:/packages:ro" -v "$repo/packaging:/packaging:ro" -v "$former:/former:ro" "$image" \
    bash -c "set -e; $install_former >/dev/null; $install_package >/dev/null; $former_gone
        bash /packaging/smoke_test.sh $version; echo 'replaced diamond-lang 0.22.0'"
