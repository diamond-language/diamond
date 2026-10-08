# logger

Write leveled text or structured JSON logs.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add logger --registry https://cuts.dilang.tech --version "^0.4.1"
facet update
```

This installs the cut into `cuts/logger/`; load it with `require_cut "logger"`.

## Usage

```ruby
require_cut "logger"

log = Logger.new("myapp")
log.info("starting up")
log.warn("low disk space")
log.error("connection failed: timeout")
# [2026-10-07 23:49:29] INFO myapp: starting up

json = Logger.new("myapp", "debug", nil, "json")
json.info("started", {"port": 8080})
# {"port":8080,"timestamp":"…","level":"info","tag":"myapp","message":"started"}
```

`Logger.new(tag, level = "info", output = nil, format = "text")`. Levels are `debug`,
`info`, `warn`, `error`, and `off`; `"json"` writes one JSON object per line, which the
[`log_viewer`](https://github.com/diamond-language/diamond/tree/main/packages/log_viewer) cut formats for reading.

## Logging every request in a server

`RequestLogging` is a [`rack`](https://github.com/diamond-language/diamond/tree/main/packages/rack) middleware that gives each request a
`request_id` and logs `request.started`, `request.completed` (with status and
`duration_ms`), or `request.failed` before re-raising. Put it first in the chain so its
timing covers the other middleware, and configure it inside the per-worker chain builder:

```ruby
require_cut "gremlin"
require_cut "rack"
require_cut "logger"

class Pages
  def self.hello(request, context)
    RequestLogging.info(request, context, "greeting", {"who": "world"})
    [200, {"Content-Type": "text/plain"}, "hello\n"]
  end
end

def build_chain()
  RequestLogging.configure("app", "info", nil, "json")
  rack_compose([RequestLogging.call], Pages.hello)
end

def app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve(8080, app)
```

Handlers log with `RequestLogging.debug/info/warn(request, context, event, fields)`; the
request's `request_id`, `method`, and `path` are attached to every line.
