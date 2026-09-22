# rack

Compose request middleware around an HTTP handler.

## Installation

Install the cut at `cuts/rack/` and load it with `require_cut "rack"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

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
response = rack_run_chain(chain, 0, some_request, {})
```

## Notes

Middleware receives `(request, context, forward)` and returns a response triple. Compose middleware with `RackChain` around a terminal `(request, context)` handler.
