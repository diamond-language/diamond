# RateLimit: a Callable[3] fixed-window rate-limiting middleware.
#
# Storage is a plain per-worker class-variable Hash (the same
# CookieSession-style pattern this package already uses for
# Thread.new-safe configuration) -- deliberately **not** shared across
# gremlin_serve(..., threads: N) workers, since each is already its own
# fully independent DiamondVm with no shared memory at all (see
# docs/threads.md). At threads: 1 (the default) or with http_serve
# (always single-threaded), this enforces the configured limit exactly.
# At threads: N, each worker enforces its own independent limit, so a
# client whose requests land across several workers (ordinary
# kernel-hashed connection distribution) could see up to N times the
# configured limit in practice -- a real, documented tradeoff of staying
# dependency-free, not an oversight. A shared limit that's actually
# correct across workers would need a store every worker's OS thread can
# see (SQLite, say), which is real additional complexity and a hard
# dependency this middleware deliberately doesn't take on.
#
# `.configure(options)` -- like SecurityHeaders/CookieSession, a
# class-variable write, so on `threads: N` it needs calling inside the
# same `build_chain()`-style function that already runs once per worker:
# - "limit": max requests allowed per window (required).
# - "window": window length in seconds (required).
# - "key": a Callable[1] `(request) -> String` identifying who to limit
#   (required, no default) -- deliberately not guessed (an X-Forwarded-
#   For header, say) since that's spoofable unless the deployment is
#   actually behind a trusted proxy stripping/overwriting it, a fact
#   this middleware has no way to know; the caller decides what
#   "identity" means for their own deployment (an IP-carrying header, an
#   API key, a session id from packages/cookies' CookieSession, or a
#   constant string for one global limit).
#
# Fixed-window counting: a key's count resets the moment `window`
# seconds have passed since that key's *first* request in the current
# window, not a rolling average -- simple and predictable, at the
# accepted cost (standard for fixed-window limiters) that a client can
# burst up to 2x the limit across a window boundary (near the end of one
# window, then again right at the start of the next).
#
# Buckets are never evicted -- one entry accumulates per distinct key
# ever seen, for the lifetime of the worker process. Fine for a bounded
# key space (a fixed set of API keys, session ids capped by concurrent
# users); an unbounded one (raw client IPs at internet scale, over a
# long-running process) will grow this Hash without bound. Out of scope
# here -- add periodic eviction yourself (sweep entries older than
# `window` on some schedule) if your own key space needs it.
class RateLimit
  def self.configure(options)
    @@limit = options["limit"]
    @@window = options["window"]
    @@key = options["key"]
    @@buckets = {}
  end

  def self.call(request, context, forward)
    key = @@key(request)
    now = Time.monotonic()
    bucket = @@buckets[key]
    if bucket == nil || now - bucket["start"] >= @@window
      bucket = {"start": now, "count": 0}
    end
    bucket["count"] = bucket["count"] + 1
    @@buckets[key] = bucket
    if bucket["count"] > @@limit
      [429, {"Content-Type": "text/plain", "Retry-After": "#{@@window}"}, "rate limit exceeded"]
    else
      forward(request, context)
    end
  end
end
