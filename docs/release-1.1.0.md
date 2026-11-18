# d6 1.1.0

1.1.0 is the first minor release of 1.x. It adds features and changes nothing that worked in
1.0: programs, projects and cuts that run on 1.0.1 run the same on 1.1.0
([compatibility](compatibility.md)).

## Files compiled into the program

`File.embed("schema.graphql")` reads a file when the program is compiled and gives its
contents as a String. A `d6 build` binary then carries its schema, SQL or templates inside it,
with nothing to install next to it.

- The path is resolved against the file the call is written in, not the working directory.
- Editing the embedded file invalidates the bytecode cache, so the next run picks it up.
- Files up to 16 MiB work. String literals still stop at 4,095 bytes.
- `--sandbox` refuses it unless `filesystem` is allowed.

See [local I/O](local-io.md#files-compiled-in-fileembed).

## Schema-first GraphQL (graphql 0.3.6)

`GraphQL::Schema.from_sdl(text)` builds a schema from a `.graphql` file, and `GraphQL::App`
binds resolver objects to it by type name. A field without a resolver method reads the
parent's method or Hash key of the same name, so only the fields that compute something need
code. `.directive("auth", function)` wraps every field the directive is applied to, which puts
access checks in one function. `GraphQL::Fail.signal(code, message)` raises an error that
carries a code. The [changelog](https://github.com/diamond-language/d6/blob/main/CHANGELOG.md)
lists the details.

graphql 0.3.6 is on [cuts.rolld6.codes](https://cuts.rolld6.codes).
Together with `File.embed`, it lets an application keep its schema in a `.graphql` file and
still ship as one binary.

## Smaller additions

- Binaries made by `d6 build` answer a lone `--version` with their name, version and the d6
  that built them, for example `pheint 45f7b2915102 (d6 1.1.0)`. The name and version come from the
  `Cutfile` next to the source; `--app-name` and `--app-version` override them
  ([deployment](deployment.md#version-of-a-built-binary)).
- `d6 --version` adds the git revision when d6 was built from a checkout other than a clean
  release tag, so a bug report from an interim build says which one. Packages and tarballs
  report the bare version.
- Modules and classes answer `respond_to?(name)` for their `def self.` functions, and
  `respond_to?` on an instance takes a String as well as a Symbol
  ([classes and modules](classes-and-modules.md#object-utilities)).

## Installing

[Getting started](getting-started.md#1-install) compares the ways to install d6. Packages
and tarballs are on the [release page](https://github.com/diamond-language/d6/releases/tag/v1.1.0).
