# rack

Compose request middleware around an HTTP handler.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add rack --registry https://cuts.dilang.tech --version "^0.1.1"
facet update
```

This installs the cut into `cuts/rack/`; load it with `require_cut "rack"`.

## Usage

A middleware is a callable taking `(request, context, forward)` and returning a
`[status, headers, body]` response. It can answer on its own or call
`forward(request, context)` to pass the request down the chain and then inspect or
change the response. The terminal handler takes `(request, context)`.

```ruby
require_cut "rack"

def logging_middleware(request, context, forward)
  response = forward(request, context)
  puts("#{request["method"]} #{request["path"]} -> #{response[0]}")
  response
end

def auth_middleware(request, context, forward)
  if request["headers"]["authorization"] == "Bearer secret"
    forward(request, context)
  else
    [401, {"Content-Type": "text/plain"}, "unauthorized"]
  end
end

def app_handler(request, context)
  [200, {"Content-Type": "text/plain"}, "hello, #{request["path"]}"]
end

chain = rack_compose([logging_middleware, auth_middleware], app_handler)
request = {"method": "GET", "path": "/", "headers": {"authorization": "Bearer secret"}}
rack_run_chain(chain, 0, request, {})
# prints "GET / -> 200"; returns [200, {"Content-Type": "text/plain"}, "hello, /"]
```

Middleware runs in the order listed, so put the one that should see every request,
such as logging, first.

## In a server

`gremlin_serve` runs each worker thread in its own VM, so build the chain inside
the dispatch function through `RackChain.get`, which builds it once per worker and
reuses it. This program serves a per-visitor counter using the logging and encrypted
session middleware from the [`logger`](https://github.com/diamond-language/diamond/tree/main/packages/logger) and [`cookies`](https://github.com/diamond-language/diamond/tree/main/packages/cookies) cuts:

```ruby
# app.di
require_cut "gremlin"
require_cut "rack"
require_cut "cookies"
require_cut "logger"

class Pages
  def self.visits(request, context)
    session = request["session"]
    count = (session["visits"] || 0) + 1
    session["visits"] = count
    [200, {"Content-Type": "text/plain"}, "visit #{count}\n"]
  end
end

def build_chain()
  RequestLogging.configure("app", "info", nil, "json")
  CookieSession.configure(secret: ENV["SESSION_SECRET"])
  rack_compose([RequestLogging.call, CookieSession.call], Pages.visits)
end

def app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve(8080, app)
```

```sh
SESSION_SECRET=$(openssl rand -hex 32) diamond app.di
curl -c jar -b jar localhost:8080/    # visit 1
curl -c jar -b jar localhost:8080/    # visit 2
```

Class methods such as `RequestLogging.call` and `Pages.visits` can be passed as
middleware and handlers; a plain top-level `def` can be passed from top-level code
but not by name from inside another `def`, which is why `Pages` is a class.

## Notes

`rack_compose` returns a plain Array of steps, `rack_run_chain(chain, index, request,
context)` runs the step at `index`, and a middleware that calls `forward` more times
than the chain has steps raises `RuntimeError`.
