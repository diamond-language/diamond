# A tiny app proving packages/rack's whole middleware set actually
# composes: SecurityHeaders, CookieSession + Csrf (packages/cookies),
# RateLimit, and Cors, all on real requests through a real gremlin_serve
# instance. Not a package itself -- see examples/library's own README
# for the precedent this follows (a small app proving pieces compose,
# not a new abstraction).
#
# Split from app.di the way examples/project_board splits its own
# boot.di from app.di: everything reachable without actually starting a
# server lives here, so smoke_test.di can `require "./boot"` and drive
# `rack_app` directly without a real socket.
#
# Two independent middleware stacks, dispatched by path prefix (rack
# itself does no routing -- see packages/rack's own README -- so this is
# the documented "put a router in front of your handler" shape, at its
# simplest: one if/else):
#
# - "/" and "/notes": a cookie-session-backed guestbook (lib/web.di).
#   SecurityHeaders -> CookieSession -> RateLimit -> Csrf -> web_handler.
#   RateLimit runs *after* CookieSession specifically because its own
#   key function (rate_limit_key, below) reads the session id
#   CookieSession just populated -- order between middlewares is not
#   arbitrary here.
# - "/api/*": a stateless, cross-origin JSON endpoint (lib/api.di).
#   SecurityHeaders -> Cors -> RateLimit -> api_status. No CookieSession/
#   Csrf: a third-party origin consuming this has no session-embedded
#   CSRF token to send.
#
# One shared RateLimit for both, not two independently configured ones:
# RateLimit.configure (like CookieSession/Cors) writes plain class
# variables, one set for the whole class, so it can only ever hold one
# policy at a time within a process -- the same "one instance per VM"
# constraint packages/rack's own README documents for RackChain, just
# not yet worked around here since one shared limit is a fine fit for
# an app this size (its own key function, below, still tells the two
# route groups' traffic apart). An app that genuinely needs two
# independently-tuned limits at once would need its own small
# multi-bucket class, the same way RackChain's own README suggests
# writing one for more than one chain.
#
# Neither chain uses RackChain's own memoization either, for the same
# root reason: RackChain also holds exactly one chain per VM, and this
# app genuinely needs two, so rebuilding each small Array fresh per
# request is simpler than writing a second memoizing class --
# rack_compose's own cost is a handful of Array pushes. That tradeoff
# only matters for `threads: N`; this example runs single-threaded
# (gremlin_serve's own default), which is also why every `.configure`
# call below happens once at the top level rather than inside a
# `build_chain()`-style function -- see packages/cookies/README.md's own
# CookieSession section for why that split exists at all once threads
# enters the picture.

require "../../packages/rack/lib/rack"
require "../../packages/cookies/lib/cookies"
require "./lib/web"
require "./lib/api"

# A real deployment sets GUESTBOOK_SESSION_SECRET; the fallback here
# exists purely so this example runs with zero setup -- see README.md.
def session_secret()
  secret = ENV["GUESTBOOK_SESSION_SECRET"]
  if secret == nil then "dev-only-insecure-secret-change-me" else secret end
end

# The one thing that has to know about both route groups: an API
# request (no session -- CookieSession never runs on this path) shares
# one bucket keyed by a constant; a web request is keyed by its own
# per-session visitor id instead, so different browsers/sessions don't
# share a limit. A real multi-consumer API would key by an API-key
# header instead of a single constant.
def rate_limit_key(request)
  if request["path"].start_with?("/api/")
    "api"
  else
    web_visitor_id(request)
  end
end

CookieSession.configure(secret: session_secret())
Cors.configure({"origins": ["https://trusted-partner.example"]})
RateLimit.configure({"limit": 30, "window": 60, "key": rate_limit_key})

def web_app(request, context)
  chain = rack_compose([SecurityHeaders.call, CookieSession.call, RateLimit.call, Csrf.call], web_handler)
  rack_run_chain(chain, 0, request, context)
end

def api_app(request, context)
  chain = rack_compose([SecurityHeaders.call, Cors.call, RateLimit.call], api_status)
  rack_run_chain(chain, 0, request, context)
end

def rack_app(request, context)
  if request["path"].start_with?("/api/")
    api_app(request, context)
  else
    web_app(request, context)
  end
end
