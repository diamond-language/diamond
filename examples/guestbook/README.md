# examples/guestbook

An end-to-end smoke test wiring together
[`packages/gremlin`](../../packages/gremlin/README.md) (server) →
[`packages/rack`](../../packages/rack/README.md) (middleware:
`SecurityHeaders`, `RateLimit`, `Cors`) →
[`packages/cookies`](../../packages/cookies/README.md) (`CookieSession`,
`Csrf`). Not a package itself, and no database -- see
[`examples/library`](../library/README.md)'s own README for the
precedent this follows (a small app proving pieces compose, not a new
abstraction).

A tiny two-route guestbook: a home page shows a per-session visit
counter and up to five recent notes (both stored entirely inside an
encrypted session cookie -- no database), with a form to post a new one.
A separate, stateless `/api/status` endpoint demonstrates the
cross-origin half of the same middleware stack.

## Layout

```
app.di           entry point: requires boot.di, starts gremlin_serve
boot.di          everything else -- rack_app, both middleware chains,
                 .configure calls -- split out so smoke_test.di can
                 require it directly without a real socket, the same
                 split examples/project_board's own boot.di/app.di use
lib/
  web.di         the cookie-session-backed guestbook routes
  api.di         the stateless, cross-origin JSON route
smoke_test.di    direct-dispatch proof of the whole composed stack
```

## What each route demonstrates

- **`GET /`** -- `SecurityHeaders -> CookieSession -> RateLimit -> Csrf`.
  Increments a per-session visit counter, renders up to five recent
  notes, and embeds the request's own CSRF token into a tiny inline
  script (there's no built-in form-body parser for a plain HTML form to
  hand a token to some other way -- see `packages/cookies`' own README
  on why `Csrf` checks a header instead). `RateLimit` runs *after*
  `CookieSession` specifically because its own key function reads the
  session id `CookieSession` just populated -- order between middlewares
  here is not arbitrary.
- **`POST /notes`** -- protected by `Csrf`; rejects a missing or wrong
  `X-CSRF-Token` with `403` before ever reaching the handler. Notes are
  capped at the 5 most recent: the whole session lives inside the
  encrypted cookie itself (`packages/cookies`' `CookieSession`), not a
  database, so an unbounded note list would be an unbounded cookie, not
  just an unbounded in-memory structure. Submitted text is HTML-escaped
  before rendering.
- **`GET /api/status`** -- `SecurityHeaders -> Cors -> RateLimit`. No
  `CookieSession`/`Csrf`: a third-party origin consuming this has no
  session-embedded CSRF token to send, and doesn't need one for a
  read-only endpoint. `Cors` is configured with an explicit allow-list
  (`https://trusted-partner.example`); a disallowed origin still gets an
  ordinary `200` (CORS is a browser-side grant, not a server-side
  reject) just without the `Access-Control-Allow-*` headers a browser
  needs to expose the response to its own cross-origin JS. An `OPTIONS`
  preflight is answered directly and never reaches the handler.

Both routes share **one** `RateLimit` policy, not two independently
tuned ones -- see `boot.di`'s own comment on why (`RateLimit.configure`,
like `CookieSession`/`Cors`, is a class-variable write, so it can only
hold one policy at a time within a process). Its key function tells the
two route groups apart: a per-session id for the web routes, a single
shared bucket for the API route.

## Run

```sh
cd examples/guestbook
../../build/diamond app.di
```

Then, in another terminal:

```sh
curl -i http://127.0.0.1:18090/                    # sets a session cookie, shows visit count 1
curl -i -b cookies.txt -c cookies.txt http://127.0.0.1:18090/   # visit count 2, same session
curl -i http://127.0.0.1:18090/api/status           # stateless JSON, no cookie
curl -i -X OPTIONS -H 'Origin: https://trusted-partner.example' \
  -H 'Access-Control-Request-Method: GET' http://127.0.0.1:18090/api/status  # CORS preflight
```

Posting a note needs the session's own CSRF token (embedded in the home
page's own inline script, not something `curl` can discover on its own)
-- open <http://127.0.0.1:18090/> in a browser and use the form there,
or read the token out of the page source for a scripted `curl -X POST`
with an `X-CSRF-Token` header matching it.

A real deployment sets `GUESTBOOK_SESSION_SECRET`; without it, `app.di`
falls back to a fixed development-only secret so the example runs with
zero setup -- see `boot.di`'s own comment.

## Smoke test

```sh
../../build/diamond smoke_test.di
```

Direct-dispatch (calls `web_app`/`api_app`/`rack_app` straight from
`boot.di`, no real socket), following `examples/project_board`'s own
`smoke_test.di` convention. Covers: session persistence across requests
via the rotating session cookie (`CookieSession` re-encrypts with a
fresh nonce on every response, so the *latest* Set-Cookie is the only
one that reflects current state -- reusing an older one silently rolls
the session back); a missing/wrong CSRF token rejected, the correct one
accepted; posted notes rendering HTML-escaped and capped at five;
security headers present even on a rejected (`403`) response; a fresh
session (no cookie) starting independent of an existing one; `RateLimit`
tripping after its configured limit; `Cors` granting headers only to an
allowed origin while still serving a disallowed one; a preflight never
reaching the handler; and the API path never touching
`CookieSession`/`Csrf` through `rack_app`'s own dispatch.
