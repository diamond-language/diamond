#!/bin/sh
# The container half of tools/build_portable.sh; runs in Alpine as root.
set -eu

apk add --no-cache bash make gcc musl-dev linux-headers git \
    openssl-dev openssl-libs-static sqlite-dev sqlite-static zlib-dev zlib-static \
    libpq-dev libucontext-dev libgcc file tar >/dev/null

# musl's crypt() has no bcrypt and Alpine packages no libxcrypt, so build
# libxcrypt's static library from a pinned, checksummed release (its
# configure needs Perl; nothing of it reaches the binary but libcrypt.a).
xcrypt_version=4.5.2
xcrypt_sha256=71513a31c01a428bccd5367a32fd95f115d6dac50fb5b60c779d5c7942aec071
apk add --no-cache perl xz >/dev/null
wget -q -O /tmp/xcrypt.tar.xz \
    "https://github.com/besser82/libxcrypt/releases/download/v$xcrypt_version/libxcrypt-$xcrypt_version.tar.xz"
echo "$xcrypt_sha256  /tmp/xcrypt.tar.xz" | sha256sum -c - >/dev/null
mkdir /tmp/xcrypt
tar -xJf /tmp/xcrypt.tar.xz -C /tmp/xcrypt --strip-components=1
(cd /tmp/xcrypt && ./configure --quiet --prefix=/opt/xcrypt --disable-shared --enable-static \
    --disable-obsolete-api && make -j"$JOBS" >/dev/null && make install >/dev/null)

# Alpine ships MariaDB Connector/C only as a shared library, so build its
# static one from a pinned, checksummed release. The authentication plugins
# a server may ask for are linked in (a static binary cannot dlopen them):
# MySQL 8's caching_sha2_password and sha256_password, MariaDB's ed25519,
# parsec and dialog; Kerberos (GSSAPI) and remote_io stay out.
mariadb_version=3.4.11
mariadb_sha256=521c0712e9291fa96558df9e2ff431376a3a79329f13751896b694cae12765b4
apk add --no-cache cmake >/dev/null
wget -q -O /tmp/mariadb.tar.gz \
    "https://archive.mariadb.org/connector-c-$mariadb_version/mariadb-connector-c-$mariadb_version-src.tar.gz"
echo "$mariadb_sha256  /tmp/mariadb.tar.gz" | sha256sum -c - >/dev/null
mkdir /tmp/mariadb
tar -xzf /tmp/mariadb.tar.gz -C /tmp/mariadb --strip-components=1
cmake -S /tmp/mariadb -B /tmp/mariadb/build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/mariadb -DWITH_UNIT_TESTS=OFF -DWITH_CURL=OFF \
    -DWITH_SSL=OPENSSL -DWITH_EXTERNAL_ZLIB=ON \
    -DCLIENT_PLUGIN_CACHING_SHA2_PASSWORD=STATIC -DCLIENT_PLUGIN_SHA256_PASSWORD=STATIC \
    -DCLIENT_PLUGIN_CLIENT_ED25519=STATIC -DCLIENT_PLUGIN_PARSEC=STATIC \
    -DCLIENT_PLUGIN_DIALOG=STATIC -DCLIENT_PLUGIN_MYSQL_CLEAR_PASSWORD=STATIC \
    -DCLIENT_PLUGIN_AUTH_GSSAPI_CLIENT=OFF -DCLIENT_PLUGIN_REMOTE_IO=OFF \
    -DCLIENT_PLUGIN_ZSTD=OFF >/dev/null
cmake --build /tmp/mariadb/build -j"$JOBS" >/dev/null
cmake --install /tmp/mariadb/build >/dev/null

mkdir /work
tar -xf /in/source.tar -C /work
cd /work

# The baseline ISA: the binary has to run on any CPU of its architecture.
case "$ARCH" in
    x86_64) march="-march=x86-64" ;;
    *) march="" ;;
esac

# libpq's static archive needs its two helper archives; musl implements no
# ucontext, which libucontext supplies. facet, diamond-lsp and diamond-dap
# compile and link in one step with CFLAGS_DEBUG (as packaging/build.sh does),
# so their static flags ride there. Two runs, as there: `release` drives its
# own sub-make, which races with the tool targets in one parallel make.
static_libs="-L/opt/xcrypt/lib -L/opt/mariadb/lib/mariadb"
set -- CC=gcc \
    CPPFLAGS_EXTRA="-I/opt/xcrypt/include -I/opt/mariadb/include/mariadb" \
    LDFLAGS_EXTRA="-static $static_libs" \
    LDLIBS_EXTRA="-lpgcommon_shlib -lpgport_shlib -lucontext" \
    CFLAGS_DEBUG="-O2 -DNDEBUG $march -static $static_libs"
make -j"$JOBS" "$@" CFLAGS_RELEASE="-O3 -DNDEBUG $march" release
make -j"$JOBS" "$@" facet lsp dap

# One directory per architecture with the four tools under their installed
# names, and the same as a release tarball.
version="$(build/diamond --version | cut -d' ' -f2)"
dir="/out/d6-linux-$ARCH"
rm -rf "$dir"
mkdir -p "$dir"
cp build/diamond "$dir/d6"
cp build/facet "$dir/facet"
cp build/diamond-lsp "$dir/d6-lsp"
cp build/diamond-dap "$dir/d6-dap"
strip "$dir"/*
for tool in "$dir"/*; do
    if ! file "$tool" | grep -q "static"; then
        echo "build_portable: $tool is not statically linked" >&2
        exit 1
    fi
done
tar -czf "/out/d6-$version-linux-$ARCH.tar.gz" -C /out --owner=0 --group=0 "d6-linux-$ARCH"
