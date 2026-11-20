# Getting started

This guide goes from installing d6 to a tested project built into a
standalone executable. Each step links to the topic guide that covers it in
depth. To try the language before installing anything, use the REPL at
[rolld6.codes](https://rolld6.codes).

## 1. Install

There are four ways to install d6, and each gives you the same four programs: `d6`, `facet`,
`d6-lsp` and `d6-dap`. They differ in where they run, what they need and how you upgrade.

|                     | Package (.rpm, .deb)             | mise, prebuilt                   | mise, asdf plugin             | From source                   |
|---------------------|----------------------------------|----------------------------------|-------------------------------|-------------------------------|
| Runs on             | Fedora 44 and 45, Ubuntu 26.04   | Linux with glibc 2.38 or later   | wherever d6 builds            | Linux, macOS, FreeBSD, Alpine |
| Installs            | system-wide, as root             | per user                         | per user                      | in a prefix of your choice    |
| Libraries           | installed by apt or dnf          | four shared libraries (below)    | development packages (below)  | development packages (below)  |
| C compiler          | only for `d6 build`              | only for `d6 build`              | GCC 15+ or Clang 19+          | GCC 15+ or Clang 19+          |
| New release         | when you download it             | 24 hours after it's published    | as soon as it's tagged        | as soon as it's tagged        |
| Several versions    | no, one per system               | yes, pinned per project          | yes, plus `main`              | one per checkout              |
| Upgrade             | install the next release's package | `mise upgrade`                 | `mise upgrade` (rebuilds)     | `git pull` and rebuild        |
| Machine code        | x86-64-v3, or any 64-bit ARM     | x86-64-v3, or any 64-bit ARM     | tuned to your CPU             | tuned to your CPU             |

A package is the simplest choice on Fedora and Ubuntu. Use mise to install without root, to
keep several versions, or to pin a version per project. Build from source on other systems
or for an unreleased version.

### From a package

Each [release](https://github.com/diamond-language/d6/releases) has a `.deb` for Ubuntu
26.04 and an `.rpm` each for Fedora 44 and Fedora 45 (`fc44` and `fc45` in the file names).
Each is built for x86-64 CPUs with AVX2 (x86-64-v3: Intel Haswell, AMD Excavator and later)
and for any 64-bit ARM CPU (`arm64` and `aarch64` in the file names). Download the one for your
system from the [latest release](https://github.com/diamond-language/d6/releases/latest) and
install it:

```sh
# Ubuntu 26.04
sudo apt update && sudo apt upgrade
sudo apt install ~/Downloads/d6-lang_1.2.0-1_amd64.deb
sudo apt install ~/Downloads/d6-lang_1.2.0-1_arm64.deb     # 64-bit ARM

# Fedora 45 (for Fedora 44, the fc44 file)
sudo dnf upgrade
sudo dnf install ~/Downloads/d6-lang-1.2.0-1.fc45.x86_64.rpm
sudo dnf install ~/Downloads/d6-lang-1.2.0-1.fc45.aarch64.rpm   # 64-bit ARM
```

The package puts `d6`, `facet`, `d6-lsp` and `d6-dap` in `/usr/bin`, and apt or dnf
installs the libraries they use. It recommends `gcc` and the libraries' development packages,
which only `d6 build` needs. apt and dnf install recommendations by default, but skip any that
would need an installed package upgraded, so upgrade first. If you didn't, `d6 build` prints
the commands that install what it's missing.

Each package is built for one distribution release and links that release's libraries, so it
isn't supported on other releases, newer ones included; use mise or a source build there.
There is no apt or dnf repository yet, so you upgrade by installing the next release's
package.

### With mise

[mise](https://mise.jdx.dev) installs d6 per user, keeps several versions side by side, and
can pin one per project in a `mise.toml`. It can install d6 two ways.

**Prebuilt.** mise downloads the release tarball for your CPU. There is no plugin to add, and
nothing is compiled:

```sh
mise use -g github:diamond-language/d6
```

It needs Linux on x86-64 (x86-64-v3, as for the packages) or 64-bit ARM, glibc 2.38 or
later (Ubuntu 24.04, Fedora 39, Debian 13 and later), and four shared libraries: SQLite,
libpq, MariaDB Connector/C and zlib:

```sh
sudo dnf install sqlite-libs libpq mariadb-connector-c zlib-ng-compat   # Fedora
sudo apt install libsqlite3-0 libpq5 libmariadb3 zlib1g                 # Ubuntu
sudo pacman -S sqlite postgresql-libs mariadb-libs zlib                 # Arch
```

OpenSSL and libcrypt are built into the programs, so the tarball works the same on
distributions with OpenSSL 3 and with OpenSSL 4 (Fedora 45, Arch). Every release is tested
on Ubuntu 26.04, Fedora 44 and Fedora 45, and on Arch for x86-64.

Things to know about the prebuilt route:

- mise holds back any version published less than 24 hours ago (its `minimum_release_age`
  setting), so `latest` gives the previous release for the first day. Name the version to get
  it at once (`mise use -g github:diamond-language/d6@1.2.0`), or set
  `MISE_MINIMUM_RELEASE_AGE=0` for one command.
- `latest` is never a release candidate. Ask for one by its full version.
- `d6 build` compiles and links your program with `gcc`, so it needs `gcc` and the development
  packages of SQLite, libpq, MariaDB Connector/C, libcrypt and zlib: the "From source" list
  below without OpenSSL, which the tarball carries.

**asdf plugin.** The [asdf-diamond](https://github.com/diamond-language/asdf-diamond) plugin
builds the release from source on your machine:

```sh
mise plugins install d6 https://github.com/diamond-language/asdf-diamond.git
mise use -g d6@latest
```

It needs everything "From source" lists below, plus `curl` and `tar`, and each install compiles
for a few minutes. In return it works wherever d6 builds, including systems the tarball doesn't
cover. It has a release as soon as the tag exists, and the programs it builds are tuned to your
CPU (the release build uses `-march=native`). `mise install d6@ref:main` builds the current `main` beside
the releases. `mise upgrade` skips `ref:` installs, so refresh one with
`mise install -f d6@ref:main`.

### Unpacked anywhere

The prebuilt route installs the release's `d6-VERSION-linux-x86_64.tar.gz` or
`d6-VERSION-linux-aarch64.tar.gz`. Without mise, download the one for your CPU, unpack it
anywhere and put its `bin/` on your `PATH`. It has the requirements listed above, and `d6`
finds the rest of the tarball relative to itself.

### From source

On other systems, or for an unreleased version, build from source. You need GCC 15+ or
Clang 19+ (d6 is C23), `make`, `git`, and the development headers for OpenSSL (3 or 4),
SQLite, PostgreSQL (libpq), MariaDB Connector/C, zlib and libcrypt. On Fedora and Ubuntu:

```sh
# Fedora
sudo dnf install git make gcc openssl-devel sqlite-devel libpq-devel \
  mariadb-connector-c-devel libxcrypt-devel zlib-devel

# Ubuntu 26.04
sudo apt install git make gcc libssl-dev libsqlite3-dev libpq-dev libmariadb-dev \
  libcrypt-dev zlib1g-dev
```

Alpine, FreeBSD and macOS commands, and how to build without the optional
libraries, are in [CONTRIBUTING.md](../CONTRIBUTING.md#build-and-test).

Build and install the interpreter, the package manager (`facet`), the language
server and the debug adapter:

```sh
git clone https://github.com/diamond-language/d6.git
cd d6
make release facet lsp dap
make install PREFIX=$HOME/.local
```

This puts `d6`, `facet`, `d6-lsp` and `d6-dap` in
`~/.local/bin` (add it to your `PATH` if it is not there already), plus the kit
`d6 build` links executables with in `~/.local/lib/diamond`.

However you installed it, check it:

```sh
d6 --version     # d6 1.2.0
```

Built from a git checkout other than a release tag, it adds the revision, as in
`d6 1.0.0-rc6-3d8eb74cab0f` (and `-dirty` with uncommitted changes); include that line in
bug reports.

## 2. The REPL

Run `d6` with no arguments in a terminal. Each line is evaluated and its
value printed, and later lines see earlier definitions:

```text
d6 REPL -- Ctrl-D, exit, or quit to leave
{> name = "Ada"
Ada
{> "Hello, #{name}!"
Hello, Ada!
{> [3, 1, 2].sort()
[1, 2, 3]
```

Type `exit` or `quit`, or press Ctrl-D on an empty line, to leave. `d6 -e CODE` runs
one expression from the shell:

```sh
d6 -e '[1, 2, 3].map() do |n| n * n end'     # [1, 4, 9]
```

Line editing, history and completion: [REPL](repl.md).

## 3. A first script

Save this as `hello.d6`:

```ruby
def greet(name: String) -> String = "Hello, #{name}!"

["Ada", "Grace"].each() do |name|
  puts(greet(name))
end
```

```sh
d6 hello.d6
```

```text
Hello, Ada!
Hello, Grace!
```

A script prints only what it writes. The REPL and `d6 -e` are different:
they also print the value of the last expression, which is why
`d6 -e '20 + 22'` prints `42` without a `puts`.

Three things trip up newcomers from other scripting languages:

- **Every call needs parentheses**: `puts("x")`, `Foo.new()`, `items.each() do ... end`.
  A bare `obj.method` is a reference to the method, not a call.
- **Types are optional, and checked.** A script that calls `puts(greet(42))`
  on its second line fails before anything runs:

  ```text
  typo.d6:2:6: error: argument 'name' of greet: expected String, got Int
  ```

- **A method whose body is one expression** can be written `def name(args) = expr`.

The language itself: [core syntax](core-syntax.md), [callables](callables.md),
[classes and modules](classes-and-modules.md),
[types and errors](types-and-errors.md), [collections](collections.md).

## 4. A project

`facet new` creates a project; after `facet install` its tests pass:

```sh
facet new cli greeter
cd greeter
facet install             # fetch minitest, the test library, into cuts/
d6 main.d6 Ada       # Hello, Ada!
bash test.sh
```

```text
greeter/
  Cutfile              the manifest: name and dependencies
  Cutfile.dev          dependencies only the tests need: minitest
  main.d6                  the entry point: passes ARGV to Greeter.main and exits with its status
  lib/greeter.d6           the code
  test/greeter_test.d6     tests for it
  test.sh                  runs every test/*_test.d6
  README.md, .gitignore
```

Other kinds: `web` (an HTTP application on gremlin and rack), `graphql` (a GraphQL API
built from a schema file), `tcp` (a line-based server), `udp`, and `lib` (a library to
publish to the registry).
All of them share this layout; see [packages](packages.md#facet-new).

`require "lib/greeter"` in `main.d6` loads `lib/greeter.d6`, relative to the
requiring file. [Source files and imports](core-syntax.md#source-files-and-imports)
covers `require` in full.

## 5. Tests

The generated test file registers each test with minitest and exits with a
status CI can check:

```ruby
require_cut "minitest"
require "../lib/greeter"

def run_tests()
  def test_greeting() = Minitest.assert_equal("Hello, Ada!", Greeter.greeting("Ada"))

  suite = Minitest.new()
  suite.test("greeting names the person", test_greeting)
  exit(suite.run() ? 0 : 1)
end
run_tests()
```

Each test is a `def` nested inside another `def`, because only a nested `def` is
a value that can be passed to `suite.test`. Change the greeting in
`lib/greeter.d6` to `"Hi, #{name}!"` and `bash test.sh` reports:

```text
FAIL: greeting names the person
  expected "Hello, Ada!", got "Hi, Ada!"
3 tests, 2 passed, 1 failed, 0 errors, 0 skipped
```

and exits 1. The other assertions (`assert`, `assert_nil`, `assert_includes`,
`assert_raises[SomeError]`, ...) and the `setup`/`teardown` hooks are described
at the top of `cuts/minitest/lib/minitest.d6`.

## 6. Dependencies

Libraries are called cuts and come from the registry at
[cuts.rolld6.codes](https://cuts.rolld6.codes). Add one, fetch it, and load it
with `require_cut`:

```sh
facet add logger
facet install
```

```ruby
require_cut "logger"

log = Logger.new("greeter")
log.info("starting")     # [2026-10-24 21:16:43] INFO greeter: starting
```

`facet add` writes the newest release as a constraint, such as `"^0.5.0"`, which
accepts any 0.5.x but not 0.6.0. `facet install` fetches what `Cutfile` asks
for into `cuts/` and records the exact versions and digests in `Cutfile.lock`.
Commit `Cutfile.lock` and leave `cuts/` out of version control (the generated
`.gitignore` does); `facet install` recreates `cuts/` from the lock, and
`facet update` moves to the newest versions the constraints allow (`facet update <name>`
just the named cuts). Constraints,
git dependencies and publishing your own cut: [packages](packages.md).

A web project starts with two dependencies, so install them before running it:

```sh
facet new web hello_web
cd hello_web
facet install
bash test.sh
d6 main.d6           # hello_web listening on http://127.0.0.1:8080
```

## 7. A web app with a database

`facet new web` creates an HTTP application; [Mjolnir](../packages/mjolnir/README.md)
maps its rows to structs. This one stores notes in SQLite:

```sh
facet new web notes
cd notes
facet add mjolnir
facet install
```

Replace `lib/notes.d6`:

```ruby
require_cut "rack"
require_cut "mjolnir"

struct Note(id: Int, title: String)
end

def build_note(row) = Note.new(row["id"], row["title"])

class Notes
  def self.notes() = Mjolnir::Schema.new("notes", {"id": :int, "title": :string}, build_note)

  def self.create_table(db)
    db.execute("CREATE TABLE IF NOT EXISTS notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL)")
  end

  # Each gremlin worker has its own context, so each opens its own connection.
  def self.repo(context)
    if context["repo"] == nil
      db = SQLite3.open(ENV["DATABASE"] == nil ? "notes.db" : ENV["DATABASE"])
      Notes.create_table(db)
      context["repo"] = Mjolnir::Repo.new(db)
    end
    context["repo"]
  end

  def self.json(status, value) = [status, {"Content-Type": "application/json"}, JSON.stringify(value)]

  def self.route(request, context)
    repo = Notes.repo(context)
    notes = Notes.notes()
    if request["method"] == "GET" && request["path"] == "/notes"
      titles = repo.all(notes.query().order_by("id")).map() do |note| note.title() end
      Notes.json(200, titles)
    elsif request["method"] == "POST" && request["path"] == "/notes"
      changeset = Mjolnir::Changeset.cast(notes, nil, {"title": request["body"]}, ["title"])
        .validate_required(["title"])
      result = repo.insert(changeset)
      case result
      when Mjolnir::Ok then Notes.json(201, {"id": result.value().id()})
      when Mjolnir::Err then Notes.json(422, result.error().errors())
      end
    else
      Notes.json(404, {"error": "not found"})
    end
  end

  def self.build_chain() = rack_compose([SecurityHeaders.call], Notes.route)
  def self.call(request, context) = rack_run_chain(RackChain.get(Notes.build_chain), 0, request, context)
end
```

```sh
d6 main.d6 &
curl -d 'milk' localhost:8080/notes      # {"id":1}
curl -d '' localhost:8080/notes          # {"title":["can't be blank"]}
curl localhost:8080/notes                # ["milk"]
```

Gremlin gives each worker a context of its own, so a worker opens its
connection on its first request and keeps it there. Tests skip the server and
the file: they hand the handler a context holding an in-memory database.
Replace `test/notes_test.d6`:

```ruby
require_cut "minitest"
require "../lib/notes"

def run_tests()
  def test_add_and_list()
    db = SQLite3.open(":memory:")
    Notes.create_table(db)
    context = {"repo": Mjolnir::Repo.new(db)}
    Notes.call({"method": "POST", "path": "/notes", "headers": {}, "body": "milk"}, context)
    [status, headers, body] = Notes.call({"method": "GET", "path": "/notes", "headers": {}}, context)
    Minitest.assert_equal("[\"milk\"]", body)
  end

  suite = Minitest.new()
  suite.test("a posted note is listed", test_add_and_list)
  exit(suite.run() ? 0 : 1)
end
run_tests()
```

Validations, updates, queries and associations are in the
[Mjolnir README](../packages/mjolnir/README.md); PostgreSQL and MySQL
connections are in [databases](databases.md).

## 8. Editor support

`d6-lsp` gives diagnostics as you type, hover, go-to-definition,
completion and formatting in any editor with a language-server client; point
it at the binary over stdio. For VS Code, install the
[d6 extension](https://marketplace.visualstudio.com/items?itemName=rolld6.d6) from the
Marketplace (search for "d6" in the Extensions view, or run
`code --install-extension rolld6.d6`). Each release also attaches it as `d6-vscode.vsix`,
for editors that install from a file.

[Language server](lsp.md), [the VS Code extension](../editors/vscode/README.md),
and [debugging](debugging.md) with breakpoints through `d6-dap`.

## 9. Build and deploy

`d6 build` compiles a program into a standalone executable that needs
neither `d6` nor the source at run time:

```sh
d6 build                  # in greeter/: builds main.d6 into ./greeter
./greeter Grace           # Hello, Grace!
```

It needs a C compiler, which links the program against the kit the package or
`make install` put in `lib/diamond`. The executable links SQLite, libpq, MariaDB Connector/C,
OpenSSL, libcrypt and zlib dynamically, so the machine it runs on needs those
runtime libraries. Fully static builds with a musl kit, cross-compiling, and
the build cache are in [deployment](deployment.md).

## Where to go next

In roughly this order:

1. The language: [core syntax](core-syntax.md), [callables](callables.md),
   [classes and modules](classes-and-modules.md),
   [types and errors](types-and-errors.md), [collections](collections.md).
2. Programs that do things: [local I/O](local-io.md), [time](time.md),
   [processes](processes.md), [networking](networking.md),
   [databases](databases.md).
3. Concurrency: [fibers](fibers.md), [threads](threads.md),
   [cancellation](cancellation.md).
4. Tooling: [packages](packages.md), [debugging](debugging.md),
   [deployment](deployment.md), [caching](caching.md), [sandbox](sandbox.md).
5. The runtime underneath: [object model](object-model.md),
   [runtime reference](runtime-reference.md).

The runnable sample applications under `examples/` and `applications/` in
[the repository](https://github.com/diamond-language/d6) show larger
programs; [the README](../README.md#repository-layout) lists them.
