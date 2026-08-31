# Graceful-shutdown state for one gremlin_worker's own accept/poll/resume
# loop, backed by class variables (@@cvar) rather than locals closed over
# by a nested def. gremlin_worker's loop already has several nested defs
# (collect_interest, resume_if_ready) that each capture every enclosing
# local/def regardless of whether they actually use it -- a hard
# 16-binding cap this codebase enforces (src/compiler.c) -- and the loop
# was already right at that limit before any of this existed, confirmed
# empirically (adding even one more captured local broke it). Class
# variables sidestep the lexical-capture mechanism entirely.
#
# Per-VM storage (docs/object-model.md's "Class variables") is exactly
# the isolation this needs anyway: each `gremlin_serve(threads: N)`
# worker thread already has its own fully independent VM (see
# `server.di`'s own "Per-worker context" comment on `context`), so these
# class variables are already correctly scoped one-per-worker with no
# extra work, the same way `context` itself already is.
class GremlinShutdown
  # No class-body-level @@cvar initializer -- matches this package's own
  # existing convention (packages/rack/lib/rack/cors.di's @@origins etc.)
  # of only ever writing a class variable from inside a method, relying
  # on an unset @@cvar reading back nil (falsy) until first written.
  def self.requested?() -> Bool = @@requested == true

  # Lazily self-arms on first read rather than requiring a separate
  # "arm the deadline" call from gremlin_worker -- the signal that sets
  # @@requested can fire, and gremlin_worker can reach a point that
  # needs a real deadline value, within the very same loop iteration
  # (the one already blocked in IO.poll when the signal arrived never
  # gets a fresh "top of loop" pass before it reaches its own
  # end-of-iteration checks) -- a plain `@@deadline` read at that point
  # would still be nil, and comparing a Float against nil is a genuine
  # runtime TypeError, not just a logic bug. Idempotent: every call
  # after the first returns the same already-armed value.
  def self.deadline()
    if @@requested == true && @@deadline == nil
      @@deadline = Time.monotonic() + 10.0
    end
    @@deadline
  end

  # gremlin_worker calls this once, right after creating its own
  # listener, purely so `request` below has something to close. Not
  # captured as a gremlin_worker local for the same 16-binding-cap
  # reason everything else here isn't.
  def self.register_listener(listener)
    @@listener = listener
  end

  # The actual Signal.trap handler (passed by singleton method
  # reference, `GremlinShutdown.request`, not called directly). Flips
  # the flag *and* closes the listener, both deliberately: IO.poll's own
  # EINTR-retry hardening (docs/networking.md) runs this handler
  # synchronously, mid-poll, then transparently retries the underlying
  # poll(2) with the exact same fd list and timeout the interrupted call
  # already had -- on an otherwise-idle server (nothing else ready),
  # that retry would just block again, indefinitely, and gremlin_worker's
  # loop body would never get a chance to re-check requested?() at all.
  # Closing the listener here gives that in-flight poll(2) a real,
  # immediate reason to return (a closed fd reports as ready/invalid),
  # not just a flag gremlin_worker won't see until some future readiness
  # event that might never come.
  def self.request()
    @@requested = true
    unless @@listener == nil
      @@listener.close()
    end
  end
end
