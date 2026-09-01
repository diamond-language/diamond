# packages/logger

A small, leveled logger for [Diamond](https://gitlab.com/dmn9180/diamond),
structured as a real, `facet`-installable package (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md))
rather than bundled into the runtime — kept in this same repo, under
`packages/`, since it has no dependents outside it to keep a separate git
history in sync with.

Replaces the ad-hoc `puts("gremlin: ...")` shape a few packages had been
hand-rolling on their own (`packages/gremlin`'s own request-handler
error path, `packages/rack`'s own README example) with one real,
shared component.

## Install

Either copy this directory straight into another project as
`cuts/logger/`, or give it its own git remote and depend on that:

```ruby
# diamond.cut, in your project
{"name": "myapp", "dependencies": {"logger": {"git": "<url-of-a-remote-for-this-directory>", "tag": "v0.1.0"}}}
```

```
$ facet install
```

## Usage

```ruby
require_cut "logger"

log = Logger.new("myapp")
log.info("starting up")
log.warn("low disk space")
log.error("connection failed: timeout")
```

```
[2026-08-25 12:00:00] INFO myapp: starting up
[2026-08-25 12:00:01] WARN myapp: low disk space
[2026-08-25 12:00:02] ERROR myapp: connection failed: timeout
```

`Logger.new(tag, level = "info", output = nil, format = "text")`:

- `tag` — the component name prefixed on every line (`"gremlin"`,
  `"myapp"`, ...), the same string the ad-hoc `puts("gremlin: ...")`
  calls this replaces already hand-wrote themselves.
- `level` — the *minimum* level this instance actually emits at, one of
  `"debug"`/`"info"`/`"warn"`/`"error"`/`"off"` (low to high severity; the
  default, `"info"`, silently drops `#debug` calls). Turn verbosity up
  or down without touching any call site:

  ```ruby
  log = Logger.new("myapp", "debug")   # everything, including #debug
  log = Logger.new("myapp", "warn")    # only #warn and #error
  log = Logger.new("myapp", "off")     # keep call sites; emit nothing
  ```
- `output` — anything responding to `.write(value)` the same way
  `File`/`TCPSocket` already do (see `docs/io.md`) — a real file to log
  to instead of the console, or a test double. Defaults to `nil`,
  meaning stdout via the ordinary `puts` builtin.

  ```ruby
  file = File.open("myapp.log", "a")
  log = Logger.new("myapp", "info", file)
  ```
- `format` — either the backward-compatible `"text"` default or `"json"`.
  JSON mode emits newline-delimited JSON (one complete object per line),
  ready for ingestion by a log viewer:

  ```ruby
  log = Logger.new("myapp", "info", nil, "json")
  log.info("request.completed", {"request_id": "req-123", "status": 200, "duration_ms": 3.4})
  ```

  ```json
  {"request_id":"req-123","status":200,"duration_ms":3.4,"timestamp":"2026-08-25T12:00:00-0700","level":"info","tag":"myapp","message":"request.completed"}
  ```

Every level method accepts an optional hash of structured fields. In JSON
mode those fields are written at the top level, while `timestamp`, `level`,
`tag`, and `message` are reserved logger metadata and cannot be overridden by
caller fields. Values and messages are escaped through `JSON.stringify`.

Every line is timestamped (`Time.now().strftime`, local time — see the
[Time guide](../../docs/time.md)) and tagged. Text mode uses the format shown
first; JSON timestamps use `%Y-%m-%dT%H:%M:%S%z`.

## `RequestLogging` — the request-timing middleware

Generalizes the near-identical `AppLogger`/`logging_middleware`/
`log_debug`/`log_info`/`log_warn` pattern independently duplicated
across `applications/skindicate.dia`, `examples/project_board`, and
`applications/pheint.dia` — a `Callable[3]` matching
[`packages/rack`](../rack/README.md)'s own middleware contract:

```ruby
require "/path/to/logger/lib/logger"

RequestLogging.configure(tag: "myapp", level: "info", format: "json")

def app_handler(request, context)
  RequestLogging.info(request, context, "widget.created", {"widget_id": 42})
  [200, {"Content-Type": "text/plain"}, "ok"]
end

def rack_app(request, context)
  chain = rack_compose([RequestLogging.call], app_handler)
  rack_run_chain(chain, 0, request, context)
end
```

`.call` mints `request["request_id"]` (`SecureRandom.hex(8)`), logs
`request.started`, then `request.completed` (with `status`/
`duration_ms`) around `forward` — or, if `forward` raises, logs
`request.failed` (`error_class`/`error_message`/`duration_ms`) before
re-raising, rather than letting the exception cross this middleware
with no record of it at all. Put it outermost in your `rack_compose`
chain so its timing covers every other middleware's own work too, not
just the route handler's.

`.debug`/`.info`/`.warn(request, context, event, fields = {})` are for
your own call sites elsewhere in the app (a controller action, an
auth check) — they tag every line with the same `request_id`/`method`/
`path` `.call` itself uses, through the same per-`context`-memoized
`Logger` (`.get(context)` — matching `Database.get`'s own per-worker
memoization pattern, needed since each `gremlin_serve(threads: N)`
worker is a fully independent VM/heap, see `docs/threads.md`).

`.correlation(context)` returns the current request's `request_id`/
`method`/`path` as a plain `Hash` (`{}` outside a request) — for code
that wants to tag its own log lines with the same request without
needing the `request` Hash itself in scope, e.g. an `ActiveRecord`
query logger correlating `database.query.*` lines with whichever
request triggered them (see `examples/project_board`'s
`build_query_logger` for the pattern this generalizes).

`examples/library`'s own `logging_middleware` isn't migrated onto this
— it's a much smaller `puts`-based two-liner with no `Logger` involved
at all, nothing this class would simplify.

## What's deliberately out of scope

- **Custom formatters.** The package supports its built-in text and JSON
  formats, but no format-string/callback hook.
- **Per-line/contextual tags beyond the Logger's own fixed `tag`.**
  Nothing like Rails' `Logger.tagged(...)` block-scoped tagging — one
  `Logger` instance has exactly one tag, set once at construction. Arbitrary
  structured fields are supported independently in JSON mode.
- **Log rotation/multiple destinations at once.** `output` is a single
  writable, full stop — layer your own rotation/fan-out on top of a
  `File`/custom writer if you need it; this package only ever calls
  `.write` on whatever you hand it.
- **UTC timestamps.** Always local time (`Time.now()`, not
  `Time.utc_now()`) — revisit if a real consumer needs otherwise.
