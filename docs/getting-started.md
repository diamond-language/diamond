# Getting started

This guide goes from installing Diamond to a tested project built into a
standalone executable. Each step links to the topic guide that covers it in
depth. To try the language before installing anything, use the REPL at
[getdiamond.codes](https://getdiamond.codes).

## 1. Install

### From a package

Each [release](https://github.com/diamond-language/diamond/releases) has a
`.deb` for Ubuntu 26.04 and an `.rpm` for Fedora 44, for x86-64 CPUs with
AVX2 (x86-64-v3: Intel Haswell, AMD Excavator and later). Download the one
for your system from the
[latest release](https://github.com/diamond-language/diamond/releases/latest)
and install it:

```sh
# Ubuntu 26.04
sudo apt install ~/Downloads/diamond-lang_0.22.0-1_amd64.deb

# Fedora 44
sudo dnf install ~/Downloads/diamond-lang-0.22.0-1.fc44.x86_64.rpm
```

The package puts `diamond`, `facet`, `diamond-lsp` and `diamond-dap` in
`/usr/bin`. It recommends `gcc` and the libraries' development packages, which
only `diamond build` needs; apt and dnf install recommendations by default.

### From source

On other systems, or for an unreleased version, build from source. You need GCC 15+ or Clang 19+ (Diamond is C23),
`make`, `git`, and the development headers for OpenSSL, SQLite, PostgreSQL
(libpq), MariaDB Connector/C, zlib and libcrypt. On Fedora and Ubuntu:

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
git clone https://github.com/diamond-language/diamond.git
cd diamond
make release facet lsp dap
make install PREFIX=$HOME/.local
```

This puts `diamond`, `facet`, `diamond-lsp` and `diamond-dap` in
`~/.local/bin` (add it to your `PATH` if it is not there already), plus the kit
`diamond build` links executables with in `~/.local/lib/diamond`.

Either way, check it:

```sh
diamond --version     # diamond 0.22.0
```

The [asdf-diamond](https://github.com/diamond-language/asdf-diamond) plugin
does the same build for asdf or mise users.

## 2. The REPL

Run `diamond` with no arguments in a terminal. Each line is evaluated and its
value printed, and later lines see earlier definitions:

```text
diamond REPL -- Ctrl-D, exit, or quit to leave
{> name = "Ada"
Ada
{> "Hello, #{name}!"
Hello, Ada!
{> [3, 1, 2].sort()
[1, 2, 3]
```

Type `exit` or `quit`, or press Ctrl-D on an empty line, to leave. `diamond -e CODE` runs
one expression from the shell:

```sh
diamond -e '[1, 2, 3].map() do |n| n * n end'     # [1, 4, 9]
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
diamond hello.d6
```

```text
Hello, Ada!
Hello, Grace!
```

A script prints only what it writes. The REPL and `diamond -e` are different:
they also print the value of the last expression, which is why
`diamond -e '20 + 22'` prints `42` without a `puts`.

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
diamond main.d6 Ada       # Hello, Ada!
bash test.sh
```

```text
greeter/
  diamond.cut              the manifest: name and dependencies
  diamond.dev.cut          dependencies only the tests need: minitest
  main.d6                  the entry point: passes ARGV to Greeter.main and exits with its status
  lib/greeter.d6           the code
  test/greeter_test.d6     tests for it
  test.sh                  runs every test/*_test.d6
  README.md, .gitignore
```

Other kinds: `web` (an HTTP application on gremlin and rack), `tcp` (a
line-based server), `udp`, and `lib` (a library to publish to the registry).
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
[cuts.dilang.tech](https://cuts.dilang.tech). Add one, fetch it, and load it
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
accepts any 0.5.x but not 0.6.0. `facet install` fetches what `diamond.cut` asks
for into `cuts/` and records the exact versions and digests in `facet.lock`.
Commit `facet.lock` and leave `cuts/` out of version control (the generated
`.gitignore` does); `facet install` recreates `cuts/` from the lock, and
`facet update` moves to the newest versions the constraints allow. Constraints,
git dependencies and publishing your own cut: [packages](packages.md).

A web project starts with two dependencies, so install them before running it:

```sh
facet new web hello_web
cd hello_web
facet install
bash test.sh
diamond main.d6           # hello_web listening on http://127.0.0.1:8080
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
diamond main.d6 &
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

`diamond-lsp` gives diagnostics as you type, hover, go-to-definition,
completion and formatting in any editor with a language-server client; point
it at the binary over stdio. For VS Code, link the extension from the
checkout:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/diamond-language
```

[Language server](lsp.md), [the VS Code extension](../editors/vscode/README.md),
and [debugging](debugging.md) with breakpoints through `diamond-dap`.

## 9. Build and deploy

`diamond build` compiles a program into a standalone executable that needs
neither `diamond` nor the source at run time:

```sh
diamond build main.d6 -o greeter
./greeter Grace           # Hello, Grace!
```

It needs a C compiler, which links the program against the kit `make install`
put in `lib/diamond`. The executable links SQLite, libpq, MariaDB Connector/C,
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

The runnable sample applications under `examples/` and `applications/` in the
checkout show larger programs; [the README](../README.md#repository-layout)
lists them.
