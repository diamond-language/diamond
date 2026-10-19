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
# Buckets of finished windows are dropped once more than "max_keys" (default
# 100000) distinct keys are held, so the table is bounded by the keys seen in
# one window rather than by every key the worker has ever seen.
class RateLimit
  def self.configure(options)
    @@limit = options["limit"]
    @@window = options["window"]
    @@key = options["key"]
    max_keys = options["max_keys"]
    @@max_keys = if max_keys == nil then 100000 else max_keys end
    @@buckets = {}
  end

  # Once more than `max_keys` distinct keys are held, drop the buckets whose window has ended (they would start a fresh count anyway). A flood of
  # distinct keys inside one window can still hold that many live buckets, so the table is bounded by the keys seen per window, not forever.
  def self.tracked_keys()
    @@buckets.length()
  end

  def self.sweep(now)
    return nil if @@buckets.length() <= @@max_keys
    live = {}
    @@buckets.keys().each() do |held|
      kept = @@buckets[held]
      live[held] = kept if now - kept["start"] < @@window
    end
    @@buckets = live
    nil
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
    RateLimit.sweep(now)
    if bucket["count"] > @@limit
      [429, {"Content-Type": "text/plain", "Retry-After": "#{@@window}"}, "rate limit exceeded"]
    else
      forward(request, context)
    end
  end
end
