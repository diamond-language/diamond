# d6 1.0.1

1.0.1 fixes installing d6 on distributions that have moved to OpenSSL 4, such as Fedora 45
and Arch. The language is unchanged from 1.0.0.

## What was broken

- **The `.rpm` was built only for Fedora 44.** It needs `libssl.so.3`, which Fedora 45
  doesn't ship, so dnf refused it there.
- **The tarballs (and so `mise use -g github:diamond-language/d6`) needed OpenSSL 3.** `d6`
  failed at start-up with `libssl.so.3: cannot open shared object file` on Fedora 45 and
  Arch unless the OpenSSL 3 compatibility package was installed. Even with it, `d6 build`
  linked its OpenSSL 3 runtime against the system's OpenSSL 4.

## What changed

- Each release now has an `.rpm` for Fedora 45 (`fc45`) as well as Fedora 44, for x86-64
  and 64-bit ARM.
- The tarballs link OpenSSL statically, so they need no `libssl` at all. The archives
  ship in the tarball's `d6 build` kit too, so building an executable needs no OpenSSL
  development package and works the same on OpenSSL 3 and 4 systems.
- `TLSSocket.connect` finds the system's CA certificates on any distribution. A statically
  linked OpenSSL looks for them where the build machine keeps them, so when that place
  doesn't exist d6 checks the usual bundle locations instead. `$SSL_CERT_FILE` and
  `$SSL_CERT_DIR` still take precedence.
- The tarball is now tested on Ubuntu 26.04, Fedora 44, Fedora 45 and Arch.

## Installing

[Getting started](getting-started.md#1-install) now compares the four ways to install d6:
packages, mise with the prebuilt tarball, mise with the asdf plugin, and building from
source. For each it lists exactly what it needs, and how it trades off root access, upgrades,
and how soon a new release appears.
