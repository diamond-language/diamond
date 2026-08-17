require "../http/http"

# A small, fiber-based, Puma-like concurrent HTTP server for Diamond.
# Pulls in packages/http's own http_parse_request/http_write_response by
# relative path (bare-name `require "http"` package resolution is
# anchored to the process's own working directory, not this file's --
# see docs/packages.md -- so a plain `require "http"` here would only
# find a sibling package actually installed as diamond_packages/http/ in
# whatever project uses gremlin, not packages/http/ two directories up
# in this repo; the relative path always resolves correctly regardless).
# `require "gremlin"` (once installed as a package the same way) then
# brings in gremlin_serve. Same Rack-style handler contract as
# http_serve --
# a Callable[1] taking a request Hash and returning [status, headers,
# body] -- but unlike http_serve's single blocking accept loop,
# gremlin_serve handles every connection concurrently: one Fiber per
# connection, driven by a top-level accept/poll/resume loop, so a slow
# client reading its response one byte at a time never blocks any other
# connection's own progress.
#
# The actual blocking I/O never happens inside a fiber. Every
# connection's socket is non-blocking (TCPServer.listen_nonblocking);
# reading or writing when nothing is ready raises WouldBlockError, which
# NonblockingConnection below catches and turns into a plain `yield` --
# suspending that connection's own fiber, in place, at whatever call
# depth it happened to be at (inside http_parse_request, inside
# http_write_response, wherever), exactly as docs/fibers.md describes.
# `gremlin_worker`'s own loop is the only thing that ever calls IO.poll
# or .resume -- it decides which suspended connections have become ready
# again and wakes exactly those, in an ordinary single-threaded event
# loop shape.
#
# `gremlin_serve(port, handler, threads: N)` runs N independent copies of
# that same event loop, each on its own OS thread (see docs/threads.md)
# with its own fully independent heap and its own listener bound to the
# same port via SO_REUSEPORT -- the default, threads: 1, is exactly the
# single-thread design above with no Thread involved at all. Concurrency
# *within* one worker is still fibers, same as ever; `threads` is a
# separate, orthogonal axis of concurrency *across* workers, for actually
# using more than one core.
#
# Deliberately basic: no keep-alive (matching http_serve's own scope
# cut), no request pipelining, no per-connection timeout (a client that
# opens a connection and never sends anything sits in `connections`
# until it disconnects or the process exits), and a request/response
# still goes through packages/http's own http_parse_request/
# http_write_response entirely unmodified -- the only new code here is
# the non-blocking connection wrapper and the event loop around it.

class NonblockingConnection
  def initialize(socket)
    @socket = socket
    @buffer = ""
    @eof = false
  end

  def socket() = @socket

  # Blocks (via yield, not the OS thread) until either more data has
  # arrived or the peer has closed -- never returns with nothing to show
  # for it. Every read/write path below funnels through this and #write's
  # own retry loop, so WouldBlockError never escapes NonblockingConnection
  # itself.
  def fill_more()
    loop do
      begin
        chunk = @socket.read(4096)
        if chunk == nil
          @eof = true
        else
          @buffer = @buffer + chunk
        end
        return nil
      rescue error: WouldBlockError
        yield
      end
    end
  end

  def gets()
    loop do
      newline = @buffer.index_of("\n")
      if newline != nil
        line = @buffer.slice(0, newline)
        if line.length() > 0 && line.slice(line.length() - 1, 1) == "\r"
          line = line.slice(0, line.length() - 1)
        end
        @buffer = @buffer.slice(newline + 1, @buffer.length())
        return line
      end
      if @eof
        if @buffer.length() == 0
          return nil
        end
        line = @buffer
        @buffer = ""
        return line
      end
      self.fill_more()
    end
  end

  # Same convention as File#read(n): reads up to n bytes and returns
  # whatever it got, including "" if the peer hit EOF with nothing left --
  # never nil (that's #gets' own EOF signal, not #read's).
  def read(n)
    while @buffer.length() < n && @eof == false
      self.fill_more()
    end
    took = @buffer.length()
    if took > n
      took = n
    end
    result = @buffer.slice(0, took)
    @buffer = @buffer.slice(took, @buffer.length())
    result
  end

  def write(value)
    remaining = value
    while remaining.length() > 0
      begin
        written = @socket.write(remaining)
        remaining = remaining.slice(written, remaining.length())
      rescue error: WouldBlockError
        yield
      end
    end
    nil
  end

  def close()
    @socket.close()
  end
end

def gremlin_worker(port, handler)
  listener = TCPServer.listen_nonblocking(port, reuse_port: true)
  connections = []

  def spawn_connection(client_socket)
    conn = NonblockingConnection.new(client_socket)
    def handle_connection()
      request = http_parse_request(conn)
      if request != nil
        response = handler(request)
        http_write_response(conn, response)
      end
      conn.close()
    end
    fiber = Fiber.new(handle_connection)
    begin
      fiber.resume()
    rescue error: StandardError
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
    def collect_interest(entry)
      read_list.push(entry["conn"].socket())
      write_list.push(entry["conn"].socket())
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
        if entry != nil
          newly_spawned.push(entry)
        end
      end
    end

    still_active = []
    def resume_if_ready(entry, position)
      readable = ready["readable"][position + 1]
      writable = ready["writable"][position]
      if (readable || writable) && entry["fiber"].alive?()
        begin
          entry["fiber"].resume()
        rescue error: StandardError
          entry["conn"].close()
        end
      end
      if entry["fiber"].alive?()
        still_active.push(entry)
      end
    end
    connections.each_with_index(resume_if_ready)
    connections = array_concat(still_active, newly_spawned)
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
def gremlin_serve(port, handler: Callable[1], threads = 1)
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
    i = i + 1
  end
  gremlin_worker(port, handler)
end
