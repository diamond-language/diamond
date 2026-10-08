# websocket

Accept and exchange WebSocket frames on a live HTTP connection.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add websocket --registry https://cuts.dilang.tech --version "^0.1.2"
facet update
```

This installs the cut into `cuts/websocket/`; load it with `require_cut "websocket"`.

## Usage

```ruby
require_cut "gremlin"
require_cut "websocket"

def handler(request, context)
  if request["path"] == "/echo" && websocket_upgrade_request?(request)
    ws = websocket_accept(context["gremlin_connection"], request)
    loop do
      message = ws.receive()
      if message == nil
        break   # peer closed the connection
      end
      ws.send_text(message["data"])   # echo it back
    end
    nil
  else
    [404, {"Content-Type": "text/plain"}, "not found"]
  end
end

gremlin_serve(8080, handler)
```

## Notes

Use a server that exposes the live connection in `context["gremlin_connection"]`. After `websocket_accept`, send and receive frames, then return `nil` from the HTTP handler.
