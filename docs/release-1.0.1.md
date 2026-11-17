# d6 1.0.1

1.0.1 fixes installing d6 on distributions that have moved to OpenSSL 4, such as Fedora 45
and Arch. The language is the same as in 1.0.0.

## What was broken in 1.0.0

- **The `.rpm` was built only for Fedora 44.** It needed `libssl.so.3`, which Fedora 45
  doesn't ship, so dnf refused to install it there.
- **The tarballs, and so `mise use -g github:diamond-language/d6`, needed OpenSSL 3.** On
  Fedora 45 and Arch, `d6` failed at start-up with `libssl.so.3: cannot open shared object
  file` unless the OpenSSL 3 compatibility package was installed. Even then, `d6 build`
  linked its OpenSSL 3 runtime against the system's OpenSSL 4.

## What changed

- Each release has an `.rpm` for Fedora 45 (`fc45`) as well as for Fedora 44, for x86-64
  and 64-bit ARM.
- The tarballs link OpenSSL statically, so they need no `libssl` at all. The tarball's
  `d6 build` kit carries the same OpenSSL, so building an executable needs no OpenSSL
  development package and works the same on OpenSSL 3 and OpenSSL 4 systems.
- `TLSSocket.connect` finds the system's CA certificates on distributions other than the
  one d6's OpenSSL was built on. A statically linked OpenSSL looks for them where the build
  machine keeps them; when that directory doesn't exist, d6 checks the usual bundle
  locations instead. `$SSL_CERT_FILE` and `$SSL_CERT_DIR` still take precedence.
- Every release tests the tarball on Ubuntu 26.04, Fedora 44 and Fedora 45, and on Arch for
  x86-64.

## Installing

[Getting started](getting-started.md#1-install) compares the four ways to install d6:
packages, mise with the prebuilt tarball, mise with the asdf plugin, and building from
source. For each it lists exactly what it needs and how it compares on root access,
upgrades and how soon a new release arrives.
