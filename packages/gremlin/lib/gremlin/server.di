# The combined poll(2) timeout (milliseconds, or -1 to block
# indefinitely) for whichever of shutdown-draining and a configured
# tick are actually in play right now -- the smaller of the two bounds
# wins, so neither one can starve the other (a tick due sooner than the
# shutdown deadline still fires during drain; a shutdown deadline
# sooner than the next tick still cuts drain off on time). A top-level
# def, not nested in gremlin_worker, since it captures nothing of its
# own -- both shutdown state and `next_tick_at` (passed in) are all it
# needs.
def gremlin_poll_timeout_ms(next_tick_at)
  shutdown_bound = if GremlinShutdown.requested?()
    remaining = GremlinShutdown.deadline() - Time.monotonic()
    if remaining < 0 then 0 else to_i(remaining * 1000.0) end
  else
    nil
  end
  tick_bound = if next_tick_at == nil
    nil
  else
    remaining = next_tick_at - Time.monotonic()
    if remaining < 0 then 0 else to_i(remaining * 1000.0) end
  end
  if shutdown_bound == nil && tick_bound == nil
    -1
  elsif shutdown_bound == nil
    tick_bound
  elsif tick_bound == nil
    shutdown_bound
  else
    if shutdown_bound < tick_bound then shutdown_bound else tick_bound end
  end
end

# `tick_interval`/`on_tick`: an optional periodic callback with no
# connection I/O of its own to hang off of -- the motivating case is
# fanning a message out to *other* gremlin_serve(threads: N) workers,
# which (per docs/threads.md) share no memory at all; the only thing
# every worker can actually see is whatever's outside the process
# entirely (a database, say). `on_tick(context)` runs on every worker
# independently, roughly every `tick_interval` seconds, with that
# worker's own per-worker `context` -- see packages/websocket's own
# README for a real use of this (cross-worker chat-room fan-out via
# polling a shared table). `on_tick` must be a zero-capture Callable,
# the same Thread.new requirement `handler` already has, since it
# crosses the same thread boundary on `threads > 1`. Both default to
# `nil`, meaning no tick at all -- IO.poll blocks with its original
# `-1` (or whatever shutdown alone already bounded it to), exactly
# today's behavior, unchanged for every existing caller.
def gremlin_worker(port, handler, tick_interval = nil, on_tick = nil)
  listener = TCPServer.listen_nonblocking(port, reuse_port: true)
  connections = []
  context = {}
  # Server errors share stdout with application logs, so they use the same
  # one-object-per-line contract instead of introducing a text-only line.
  log = Logger.new("gremlin", "info", nil, "json")
  next_tick_at = if tick_interval == nil then nil else Time.monotonic() + tick_interval end

  # Graceful shutdown on SIGTERM/SIGINT ("someone asked this process to
  # stop" -- Signal.trap's own motivating use case, docs/networking.md).
  # State lives in GremlinShutdown's class variables, not a local here --
  # see that file's own comment for why (gremlin_worker's nested defs
  # were already right at this codebase's 16-binding lexical-capture
  # cap). Passed by singleton method reference (no wrapping def needed
  # here). GremlinShutdown.request both flags the shutdown *and* closes
  # `listener` itself -- see that method's own comment for why closing
  # it is required, not optional: IO.poll's EINTR-retry hardening
  # transparently retries the exact same blocked poll(2) after running
  # this handler, so without something making that specific poll(2)
  # actually return, an otherwise-idle server would never notice the
  # signal at all.
  GremlinShutdown.register_listener(listener)
  Signal.trap("TERM", GremlinShutdown.request)
  Signal.trap("INT", GremlinShutdown.request)

  def spawn_connection(client_socket)
    conn = NonblockingConnection.new(client_socket)
    def handle_connection()
      request = http_parse_request(conn)
      unless request == nil
        # context["gremlin_connection"]: this request's own live
        # NonblockingConnection -- an escape hatch for a handler that
        # needs the raw connection itself, not just a [status, headers,
        # body] response value (a WebSocket upgrade being the motivating
        # case: packages/websocket writes its own 101 handshake, then
        # takes over the connection for framed I/O directly, for as
        # long as that WebSocket stays open). Ordinary request handling
        # never needs this at all -- it's here purely for a handler
        # that wants to opt in.
        context["gremlin_connection"] = conn
        response = handler(request, context)
        # nil is the signal a handler already fully took over `conn`
        # itself (via context["gremlin_connection"] above) and wrote
        # its own response directly -- no ordinary handler could
        # previously return nil (every existing one always returns a
        # real [status, headers, body]), so this is a new, backward-
        # compatible meaning, not a reinterpretation of anything that
        # used to mean something else.
        unless response == nil
          http_write_response(conn, response)
        end
      end
      conn.close()
    end
    fiber = Fiber.new(handle_connection)
    begin
      fiber.resume()
    rescue error: StandardError
      log.error("request.handler_failed", {"error": error.message()})
      conn.close()
      return nil
    end
    if fiber.alive?()
      {"conn": conn, "fiber": fiber}
    else
      nil
    end
  end

  loop do
    if GremlinShutdown.requested?() && connections.length() == 0
      log.info("server.shutdown_complete", {"forced": false})
      exit(0)
    end

    # Once shutdown is requested, `listener` has already been closed (by
    # GremlinShutdown.request itself) and must not be polled again --
    # IO.poll rejects a closed listener outright (DIAMOND_VM_IO_ERROR,
    # src/vm.c's pollable_fd), and there's no reason to accept new
    # connections during drain anyway. resume_if_ready below reads its
    # own matching offset back out of GremlinShutdown.requested?()
    # rather than a captured local, for the same reason as everywhere
    # else in this function.
    read_list = if GremlinShutdown.requested?() then [] else [listener] end
    write_list = []
    # A connection only goes into write_list while its own #write is
    # mid-flight (NonblockingConnection#want_write?) -- POLLOUT is
    # almost always ready for an idle socket, so including every open
    # connection unconditionally (as this used to) meant IO.poll's
    # "block until something's ready" call never really blocked, busy-
    # spinning resume_if_ready across every connection on every tick and
    # starving the listener's own accept() processing under concurrent
    # load. write_positions mirrors `connections` 1:1 (nil where that
    # connection isn't in write_list this tick) so resume_if_ready below
    # can still look up its writable result by position.
    write_positions = []
    def collect_interest(entry)
      read_list.push(entry["conn"].socket())
      if entry["conn"].want_write?()
        write_positions.push(write_list.length())
        write_list.push(entry["conn"].socket())
      else
        write_positions.push(nil)
      end
    end
    connections.each(collect_interest)
    # While shutting down, or with a tick configured, poll with a
    # bounded timeout instead of -1 so the loop keeps waking up to
    # re-check the deadline/tick even if nothing else becomes ready --
    # otherwise a client that opened a connection and never sent
    # anything (packages/gremlin's own documented "nothing evicts it"
    # scope cut) would wedge shutdown forever waiting on a poll() that
    # never returns, and a tick would never fire at all on an otherwise
    # idle worker. See gremlin_poll_timeout_ms's own comment for how the
    # two bounds combine.
    ready = IO.poll(read_list, write_list, gremlin_poll_timeout_ms(next_tick_at))

    # Accepted here, *not* folded into `connections` until after the resume
    # pass below -- `ready`'s own readable/writable arrays are sized and
    # ordered to match `connections` exactly as it was when IO.poll ran
    # above, so mutating connections before that pass would misalign every
    # position + 1 / position lookup against a newly-grown array (a real
    # bug caught here: an accepted connection that was handled and closed
    # within its own first resume() no-ops either way, but one that
    # *doesn't* finish immediately used to throw the very next connection
    # added after it out of alignment with `ready`, an IndexError at
    # random depending on accept timing).
    #
    # `ready["readable"][0]` only means "the listener is ready to
    # accept" while the listener is actually in read_list this tick
    # (see above) -- reading it when shutting down and read_list was
    # `[]` instead just reads connections[0]'s own readiness, which the
    # `&& !GremlinShutdown.requested?()` short-circuit below always
    # discards without ever calling .accept(), so no closed-listener
    # access happens either way.
    #
    # This guard alone doesn't fully rule out a closed listener inside
    # the loop below, though: GremlinShutdown.requested?() being false
    # here only proves the signal hadn't arrived *yet* at this exact
    # instruction -- Diamond dispatches a pending trapped signal at
    # up to once per bytecode instruction (docs/networking.md), and
    # several run between this check and any given .accept() call
    # inside the loop, so the signal (and the listener close it
    # triggers) can still land mid-loop. rescue error: IOError below
    # treats that race the same as the loop's own ordinary termination
    # (client_socket == nil) -- a real, if narrow, empirically-found
    # race, not a hypothetical one.
    newly_spawned = []
    if ready["readable"][0] && !GremlinShutdown.requested?()
      begin
      loop do
        client_socket = listener.accept()
        if client_socket == nil
          break
        end
        entry = spawn_connection(client_socket)
        unless entry == nil
          newly_spawned.push(entry)
        end
      end
      rescue error: IOError
        nil
      end
    end

    still_active = []
    def resume_if_ready(entry, position)
      # +1 only while read_list actually held the listener at index 0
      # *when IO.poll ran* -- derived from read_list/connections'
      # own lengths (both already fixed for this tick) rather than a
      # fresh GremlinShutdown.requested?() re-check here: the signal
      # (and the requested-flag flip it causes) can land at any
      # bytecode instruction between read_list's construction above and
      # this point, so re-checking live can disagree with what read_list
      # actually contained when `ready` was computed -- exactly the kind
      # of misalignment this whole function's own surrounding comments
      # already warn about, just from a new source (signal timing, not
      # connection accept timing).
      readable_offset = read_list.length() - connections.length()
      readable = ready["readable"][position + readable_offset]
      write_position = write_positions[position]
      writable = if write_position == nil then false else ready["writable"][write_position] end
      if (readable || writable) && entry["fiber"].alive?()
        begin
          entry["fiber"].resume()
        rescue error: StandardError
          log.error("request.handler_failed", {"error": error.message()})
          entry["conn"].close()
        end
      end
      if entry["fiber"].alive?()
        still_active.push(entry)
      end
    end
    connections.each_with_index(resume_if_ready)
    connections = still_active.concat(newly_spawned)

    # Checked once per loop iteration rather than on some separate
    # timer -- gremlin_poll_timeout_ms above already guarantees the
    # loop wakes up (via IO.poll returning empty) no later than
    # `next_tick_at`, even with zero connection activity, so this is
    # never actually late by more than however long one connection's
    # own resume_if_ready pass just took.
    if next_tick_at != nil && Time.monotonic() >= next_tick_at
      begin
        on_tick(context)
      rescue error: StandardError
        log.error("tick.handler_failed", {"error": error.message()})
      end
      next_tick_at = Time.monotonic() + tick_interval
    end

    if GremlinShutdown.requested?() && connections.length() == 0
      log.info("server.shutdown_complete", {"forced": false})
      exit(0)
    end
    if GremlinShutdown.requested?() && Time.monotonic() >= GremlinShutdown.deadline()
      log.info("server.shutdown_complete",
        {"forced": true, "remaining_connections": connections.length()})
      exit(0)
    end
  end
end

# `threads = 1` (the default) is exactly today's behavior: no Thread.new
# calls, gremlin_worker runs inline on the calling thread. `threads > 1`
# spawns `threads - 1` additional OS threads, each running its own fully
# independent copy of gremlin_worker's own accept/poll/resume loop -- no
# shared state between them at all (Thread's whole design), including no
# shared Listener: every worker opens its own, all bound to the same port
# via reuse_port: true above, so the kernel load-balances new connections
# across them. `gremlin_worker` is itself a top-level def, and `handler`
# must be a zero-capture Callable, satisfying Thread.new's requirement for
# both its primary callable and (as of the reuse_port work) a crossable
# argument -- see docs/threads.md. `tick_interval`/`on_tick` (see
# gremlin_worker's own doc comment) cross the same way: `on_tick` must
# also be a zero-capture Callable, and each spawned worker gets its own
# independent tick, not one shared across `threads > 1` -- exactly the
# same per-worker-everything story as `context` itself.
#
# Pass `threads:` explicitly whenever calling with `tick_interval:`/
# `on_tick:` (even to just restate the default, `threads: 1`) --
# Diamond's own keyword-argument rule (docs/callables.md) lets a
# keyword fill any parameter, but only if every default-valued
# parameter *before* the highest one actually supplied is *also*
# supplied; skipping `threads` while naming `tick_interval`/`on_tick`
# leaves a gap and fails to compile ("missing argument"), not a bug in
# this function itself.
def gremlin_serve(port, handler: Callable[2], threads = 1, tick_interval = nil, on_tick = nil)
  if threads < 1
    raise ArgumentError.new("gremlin_serve threads must be at least 1")
  end
  # Every spawned worker's Thread handle must stay referenced for as long
  # as the server runs -- Thread.new's return value is otherwise a plain
  # GC-reachable object like any other (see docs/threads.md's "GC and
  # lifecycle"), and `free_thread` (src/vm.c) block-joins an unreferenced
  # Thread's real OS thread the moment it's collected, to guarantee no OS
  # thread ever outlives its handle. gremlin_worker's own loop is
  # deliberately infinite (a real server, never returns), so a GC on this
  # thread that reaped a discarded worker handle would block forever
  # waiting for a thread that's never going to exit -- reproduced as a
  # near-guaranteed hang on `gremlin_worker(port, handler)` below's own
  # first allocation, since `next_gc` starts at a tiny 2048-byte
  # threshold. `spawned` only needs to keep each handle reachable, never
  # read again -- this stack frame itself never returns (this function's
  # own last line runs gremlin_worker inline, forever), so `spawned`
  # stays a live GC root for the server's entire lifetime.
  spawned = []
  (threads - 1).times() do |i|
    spawned.push(Thread.new(gremlin_worker, port, handler, tick_interval, on_tick))
  end
  gremlin_worker(port, handler, tick_interval, on_tick)
end
