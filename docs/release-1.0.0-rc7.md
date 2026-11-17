# d6 1.0.0-rc7

The seventh release candidate changes nothing in the language or runtime: d6 itself is
the same as 1.0.0-rc6. It adds two ways to install d6, and it runs the release process
that 1.0.0 will use from start to finish.

## Install with mise, or unpack anywhere

Each release now has `d6-VERSION-linux-x86_64.tar.gz` and `d6-VERSION-linux-aarch64.tar.gz`
beside the `.deb` and `.rpm`. They hold the same programs as the packages, in a directory
you can unpack anywhere, and run on current glibc distributions with OpenSSL 3.
[mise](https://mise.jdx.dev) installs from them, with no plugin and no compiler:

<!-- doc-check: skip -->
```sh
mise use -g github:diamond-language/d6
```

[Getting started](getting-started.md#with-mise) lists what the
tarballs need, and what to do on distributions that have moved to OpenSSL 4.

## The VS Code extension in the Marketplace

The VS Code extension (highlighting, diagnostics, hover, go-to-definition, completion and
debugging) is published to the Visual Studio Marketplace as `rolld6.d6`, and each release
also attaches it as a `.vsix`. Before, it had to be linked in from a source checkout.
