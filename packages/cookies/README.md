# cookies

Parse and serialize cookies, and provide signed cookies, encrypted cookies, sessions, CSRF tokens, and flash messages.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add cookies --registry https://cuts.dilang.tech --version "^0.1.1"
facet update
```

This installs the cut into `cuts/cookies/`; load it with `require_cut "cookies"`.

## Usage

Parsing and writing cookie headers:

```ruby
require_cut "cookies"

cookie_parse(request["headers"]["cookie"])
# => {"session_id": "abc123", "theme": "dark"}

cookie_serialize("session_id", "abc123", {"path": "/", "max_age": 3600})
# => "session_id=abc123; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax"
```

`cookie_serialize` returns one `Set-Cookie` value. Set `secure: true` when serving
over HTTPS.

Tamper-proof and private values, keyed by a secret you keep in the environment:

```ruby
signed = SignedCookies.sign("user=7", ENV["COOKIE_SECRET"])
SignedCookies.verify(signed, ENV["COOKIE_SECRET"])   # => "user=7", or nil if altered

hidden = EncryptedCookies.encrypt("user=7", ENV["COOKIE_SECRET"])
EncryptedCookies.decrypt(hidden, ENV["COOKIE_SECRET"])   # => "user=7", or nil
```

## Sessions, CSRF tokens and flash messages in a server

`CookieSession`, `Csrf`, and `Flash` are request middleware (and helpers) for the
[`rack`](https://github.com/diamond-language/diamond/tree/main/packages/rack) cut's chains. `CookieSession.call` reads the encrypted session
cookie into `request["session"]`, a Hash your handlers read and write freely, and
writes it back as `Set-Cookie` on the way out:

```ruby
require_cut "gremlin"
require_cut "rack"
require_cut "cookies"

class Pages
  def self.save(request, context)
    Flash.set(request, "notice", "Saved")
    [302, {"Location": "/"}, ""]
  end

  def self.home(request, context)
    notice = Flash.get(request, "notice")        # set by the previous request, once
    token = Csrf.token(request)                  # put this in your forms
    [200, {"Content-Type": "text/plain"}, "notice=#{notice} csrf=#{token}\n"]
  end

  def self.route(request, context)
    if request["method"] == "POST" then Pages.save(request, context) else Pages.home(request, context) end
  end
end

def build_chain()
  CookieSession.configure(secret: ENV["SESSION_SECRET"])
  rack_compose([CookieSession.call, Csrf.call], Pages.route)
end

def app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve(8080, app)
```

- `CookieSession.configure(secret:, cookie_name: "_session")` must run inside the
  per-worker `build_chain`, as above, because each `gremlin_serve` worker has its own VM.
- `Csrf.call` allows `GET`, `HEAD`, and `OPTIONS`; any other method must send the
  session's token in an `x-csrf-token` request header, or it gets a 403. A plain HTML
  form needs a little script that copies `Csrf.token(request)` into that header.
- `Flash.set` stores a value for the next request only; a value set and read within
  one request reads back `nil`.
