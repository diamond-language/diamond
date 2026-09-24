# examples/logstat

A command-line tool that summarizes newline-delimited JSON logs, such as the
structured output of the [`logger`](../../packages/logger/README.md) cut,
gremlin, and the package registry. It runs under the interpreter or ships as
one binary built with `diamond build`.

```text
$ diamond build logstat.di
$ ./logstat testdata/sample.ndjson
9 lines, 2 unparsed

Levels
  debug          1
  info           3
  warn           2
  error          1

Requests: 4
  2xx 2  3xx 0  4xx 1  5xx 1
  latency ms  p50 1.5  p95 40.0  p99 40.0  max 40.0

Top messages
         4  registry request.completed
         1  registry server.starting
         1  gremlin request.rejected
         1  jobs job.claimed

Unparsed lines
  line 6: not JSON
  line 9: not a JSON object
```

## Usage

```text
logstat [--top N] [--strict] [FILE...]
```

- Reads each `FILE` in order, or stdin when there are none (or for `-`).
- `--top N` limits the message and unparsed-line lists (default 5).
- `--strict` exits 1 when any line couldn't be parsed.
- Exit status: 0 on success, 1 for `--strict` failures, 64 for a usage error,
  and 66 when a file can't be opened.

A line counts as a request when its message is `request.completed` and it has
an integer `status` and a numeric `duration_ms`. Latency percentiles use the
nearest-rank method.

## What it shows

- **A closed set of line types.** `lib/lines.di` declares `sealed class
  LogLine` with three subclasses. `Tally#record` (`lib/report.di`) matches on
  them with `case`; because `LogLine` is sealed, leaving a subclass out is a
  compile error, and inside each `when` the line is narrowed to that subclass.
- **Structs for plain data.** `Latency` is a `struct`, so it gets its
  constructor, readers, equality, and `to_s` for free.
- **Typed signatures.** Parameters and results such as
  `logstat_parse(text: String, number: Int) -> LogLine` are checked at
  compile time.
- **A real CLI.** Argument parsing from `ARGV`, file and stdin input, and
  conventional exit codes through `exit`.
- **Shipping one file.** `diamond build logstat.di` produces a standalone
  executable; `smoke_test.sh` checks that it prints exactly what the
  interpreter prints.

It needs no cuts, so there is no `diamond.cut`.

## Test

```sh
bash smoke_test.sh
```

## Notes

- Diamond has no stderr writer for the running program yet, so errors are
  appended to `/dev/stderr` (Linux, macOS, and FreeBSD).
- `Float` has no rounding methods; `logstat_ms` rounds to one decimal place
  by hand.
