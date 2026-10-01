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
# class variables remain scoped one-per-worker, like `context`. Managed
# workers explicitly configure their shared cancellation/deadline channels.
class GremlinShutdown
  # No class-body-level @@cvar initializer -- matches this package's own
  # existing convention (packages/rack/lib/rack/cors.di's @@origins etc.)
  # of only ever writing a class variable from inside a method, relying
  # on an unset @@cvar reading back nil (falsy) until first written.
  def self.configure(token, timeout)
    unless timeout is Int || timeout is Float
      raise ArgumentError.new("shutdown_timeout must be finite and nonnegative")
    end
    if timeout < 0 || !timeout.to_f().finite?()
      raise ArgumentError.new("shutdown_timeout must be finite and nonnegative")
    end
    @@token_managed = token != nil
    @@channels = if token == nil then [] else token.wait_channels() end
    @@token_deadline = if token == nil then nil else token.wait_deadline() end
    @@grace = timeout
    @@shared_deadline = nil
    @@stop = nil
  end
  def self.configure_group(channels, deadline, timeout, shared_deadline, stop)
    @@token_managed = true
    @@channels = channels.concat([stop])
    @@token_deadline = deadline
    @@grace = timeout
    @@shared_deadline = shared_deadline
    @@stop = stop
    self.return_after_shutdown(true)
  end
  def self.token_managed?() = @@token_managed == true

  def self.begin_drain()
    if @@requested == true then return nil end
    @@requested = true
    @@deadline = Time.monotonic() + (if @@grace == nil then 10.0 else @@grace end)
    if @@shared_deadline != nil
      # A capacity-one mailbox serializes the first observer's deadline.
      # Every worker returns the value for the next reader, including late starters.
      shared = @@shared_deadline.receive()
      if shared != nil then @@deadline = shared end
      @@shared_deadline.send(@@deadline)
      @@stop.close()
    end
    unless @@listener == nil then @@listener.close() end
    Logger.new("gremlin", "info", nil, "json").info("server.shutdown_started", {})
  end

  def self.observe_token()
    if !self.token_managed?() || @@requested == true then return nil end
    @@channels.each() do |channel|
      if channel.closed?() then self.begin_drain() end
    end
    if @@token_deadline != nil && Time.monotonic() >= @@token_deadline
      self.begin_drain()
    end
  end

  def self.poll(readables, writables, timeout_ms)
    unless self.token_managed?() then return IO.poll(readables, writables, timeout_ms) end
    deadline = if timeout_ms < 0 then nil else Time.monotonic() + timeout_ms.to_f() / 1000.0 end
    # Once draining, the cancelled token must not keep waking poll in a loop.
    # Only the drain/request/tick deadlines and live sockets matter then.
    token_deadline = if @@requested == true then nil else @@token_deadline end
    if token_deadline != nil && (deadline == nil || token_deadline < deadline)
      deadline = token_deadline
    end
    channels = if @@requested == true then [] else @@channels end
    IO.poll(readables, writables, {"cancellations": channels, "deadline": deadline})
  end

  def self.return_after_shutdown(value)
    @@return_after_shutdown = value
  end
  def self.return_after_shutdown?() -> Bool = @@return_after_shutdown == true
  def self.close_connections(connections)
    connections.each() do |entry| entry["conn"].close() end
    # Resume each I/O-suspended handler once so the closed socket raises and
    # its ensure blocks run. Arbitrary handler code must still cooperate.
    connections.each() do |entry|
      if entry["fiber"].alive?()
        begin
          entry["fiber"].resume()
        rescue error: StandardError
          nil
        end
      end
    end
  end

  def self.requested?() -> Bool = @@requested == true

  # Anchored when draining starts, never extended by poll retries.
  def self.deadline() = @@deadline

  # gremlin_worker calls this once, right after creating its own
  # listener, purely so `request` below has something to close. Not
  # captured as a gremlin_worker local for the same 16-binding-cap
  # reason everything else here isn't.
  def self.register_listener(listener)
    @@requested = false
    @@deadline = nil
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
  #
  # A *second* SIGTERM/SIGINT (an impatient double Ctrl+C, or an
  # operator who doesn't want to wait out however much of the 10s grace
  # period is left) is treated as "stop being nice" -- exits right here,
  # synchronously inside the handler itself, rather than requiring
  # gremlin_worker's loop to notice anything. Safe to do from here:
  # exit() is an immediate, unconditional process termination (flushes
  # stdout/stderr first, src/vm.c's own exit_helper) with nothing after
  # it that depends on returning normally, unlike closing the listener
  # above -- there's no equivalent "something else needs to observe
  # this cleanly" concern the way there was for that. A fresh
  # Logger.new(...) here (rather than the one gremlin_worker's own
  # scope already has) is the same 16-binding-cap reason as everything
  # else in this file -- this handler can't reach gremlin_worker's
  # locals at all, only what it's given (@@listener) or constructs
  # itself.
  def self.request()
    if @@requested == true
      Logger.new("gremlin", "info", nil, "json").info(
        "server.shutdown_forced_by_signal", {})
      exit(0)
    end
    self.begin_drain()
  end
end
