# d6 1.2.0

1.2.0 adds what three small applications ran short of: a chat server, an uptime monitor and
an expense tracker that imports bank CSV files. Programs, projects and cuts that run on
1.1.0 run the same on 1.2.0 ([compatibility](compatibility.md)), with two exceptions noted
below: a closed output pipe now ends a program quietly, and `facet update` takes newer
compatible releases.

## An API reference for the builtins

The [API reference](api/index.md) has one entry for every builtin function and method, with
its signature and an example. CI runs every example. A docs check fails when a builtin has no
entry, or when an entry names something that does not exist.

## Text, dates and files

- `Regexp#match_data` and `String#match_data` return a `MatchData`: groups by number, name
  or Symbol, `pre_match`, `post_match`, `offset` and the captures. A Regexp prints as
  `/pattern/flags`. `match` still returns an Array ([MatchData](api/match_data.md)).
- `Time.strptime(text, format)` reads a date or time in a `strftime`-style layout such as
  `"%m/%d/%Y"` or `"%d %b %Y %I:%M %p"` ([Time](api/time.md)).
- `Dir.mktmpdir` makes a private temporary directory. `Dir.delete_tree` removes a directory
  and everything in it, and never follows a symlink ([Dir](api/dir.md)).
- `URI` percent-encodes for URLs and reads and writes form bodies and query strings
  ([URI](api/uri.md)).

## Standard streams and the process

- `STDOUT`, `STDERR` and `STDIN` are values, so a function can take where to write as an
  argument. `STDERR.print` writes without a newline, which `warn` cannot
  ([Process, Signal, streams and IO](api/process.md#stdout-stderr-and-stdin)).
- `sleep(seconds)` takes an Int or a Float. Trapped signals run during the pause.
- `Process.pid()` returns the process id.
- When the program reading a d6 program's output stops early, as `d6 report.d6 | head` does,
  the program ends with status 141 and prints nothing, like a C tool killed by SIGPIPE. It
  used to report an uncaught `IOError` with a backtrace. The `IOError` is still raised
  first, so `rescue` and `ensure` run.

## Objects

- `equal?(other)` asks whether two values are the same object, whatever `==` their class
  defines.
- A class or module body can set a class variable (`@@cache = {}` beside the `def`s). Like a
  constant, each new `Thread` starts with a copy of it.
- `respond_to?` works on native objects such as Channel, File, Regexp, Thread, Time, sockets
  and database handles.

## Projects and cuts

- `facet new graphql <path>` creates a GraphQL API with its schema compiled in.
- `facet update` moves every cut to the newest release its constraint allows. Before, it
  kept each locked version that still fit. `facet update <name>...` moves only the named
  cuts. `facet install` still keeps locked versions when it re-resolves, so adding one
  dependency upgrades nothing else ([packages](packages.md)).
- New cuts on [cuts.rolld6.codes](https://cuts.rolld6.codes):
  - `csv` 0.1.0: RFC 4180 reading and writing, streaming with `CSV.foreach`.
  - `toml` 0.1.0, now 0.1.1: TOML 1.0 parsing and generation.
- Updated cuts:
  - jobs 0.2.4: `Jobs.create_tables(db)` creates the queue's tables.
  - active_record 0.21.4: `create` returns the new row's id. It returned the affected row
    count, 1.
  - websocket 0.2.4: a client for `ws://` and `wss://` URLs.
  - gremlin 0.5.7: a `limits` Hash may give only some of its keys.
  - The [changelog](https://github.com/diamond-language/d6/blob/main/CHANGELOG.md) lists
    every cut change, including `HEAD` support in gremlin, http and rack.

## Fixes

- A `TCPSocket` sends each `write` at once. Before, writes waited for a `flush`, a read or
  `close`, which could deadlock two peers.
- `IO.poll` no longer restarts its timeout after a trapped signal's handler runs.

## Installing

[Getting started](getting-started.md#1-install) compares the ways to install d6. Packages
and tarballs are on the [release page](https://github.com/diamond-language/d6/releases/tag/v1.2.0).
