# cookies

Parse and serialize cookies, and provide signed cookies, encrypted cookies, sessions, CSRF tokens, and flash messages.

## Installation

Install the cut at `cuts/cookies/` and load it with `require_cut "cookies"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "cookies"

cookie_parse(request["headers"]["cookie"])
# => {"session_id": "abc123", "theme": "dark"}

cookie_serialize("session_id", "abc123", {"path": "/", "max_age": 3600})
# => "session_id=abc123; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax"
```

## Notes

`cookie_serialize` returns one `Set-Cookie` value. Set `secure: true` when serving over HTTPS. `CookieSession`, `Csrf`, and `Flash` provide request middleware helpers.
