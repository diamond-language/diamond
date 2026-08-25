def gremlin_worker(port, handler)
  listener = TCPServer.listen_nonblocking(port, reuse_port: true)
  connections = []
  context = {}
  # Server errors share stdout with application logs, so they use the same
  # one-object-per-line contract instead of introducing a text-only line.
  log = Logger.new("gremlin", "info", nil, "json")

  def spawn_connection(client_socket)
    conn = NonblockingConnection.new(client_socket)
    def handle_connection()
      request = http_parse_request(conn)
      unless request == nil
        response = handler(request, context)
        http_write_response(conn, response)
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
    read_list = [listener]
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
    ready = IO.poll(read_list, write_list, -1)

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
    newly_spawned = []
    if ready["readable"][0]
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
    end

    still_active = []
    def resume_if_ready(entry, position)
      readable = ready["readable"][position + 1]
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
# argument -- see docs/threads.md.
def gremlin_serve(port, handler: Callable[2], threads = 1)
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
  i = 1
  while i < threads
    spawned.push(Thread.new(gremlin_worker, port, handler))
    i += 1
  end
  gremlin_worker(port, handler)
end
