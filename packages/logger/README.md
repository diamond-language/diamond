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

`Logger.new(tag, level = "info", output = nil)`:

- `tag` — the component name prefixed on every line (`"gremlin"`,
  `"myapp"`, ...), the same string the ad-hoc `puts("gremlin: ...")`
  calls this replaces already hand-wrote themselves.
- `level` — the *minimum* level this instance actually emits at, one of
  `"debug"`/`"info"`/`"warn"`/`"error"` (low to high severity; the
  default, `"info"`, silently drops `#debug` calls). Turn verbosity up
  or down without touching any call site:

  ```ruby
  log = Logger.new("myapp", "debug")   # everything, including #debug
  log = Logger.new("myapp", "warn")    # only #warn and #error
  ```
- `output` — anything responding to `.write(value)` the same way
  `File`/`TCPSocket` already do (see `docs/io.md`) — a real file to log
  to instead of the console, or a test double. Defaults to `nil`,
  meaning stdout via the ordinary `puts` builtin.

  ```ruby
  file = File.open("myapp.log", "a")
  log = Logger.new("myapp", "info", file)
  ```

Every line is timestamped (`Time.now().strftime`, local time — see
`docs/io.md`'s own "Time" section) and tagged, matching the format
shown above exactly; there's no way to change the line format itself in
this first version (see "What's deliberately out of scope" below).

## What's deliberately out of scope

- **A configurable line format.** The `[timestamp] LEVEL tag: message`
  shape is fixed — no format-string/callback hook to customize it.
  Revisit if a real consumer needs structured (e.g. JSON) output.
- **Per-line/contextual tags beyond the Logger's own fixed `tag`.**
  Nothing like Rails' `Logger.tagged(...)` block-scoped tagging — one
  `Logger` instance has exactly one tag, set once at construction.
- **Log rotation/multiple destinations at once.** `output` is a single
  writable, full stop — layer your own rotation/fan-out on top of a
  `File`/custom writer if you need it; this package only ever calls
  `.write` on whatever you hand it.
- **UTC timestamps.** Always local time (`Time.now()`, not
  `Time.utc_now()`) — revisit if a real consumer needs otherwise.
