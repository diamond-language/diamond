require "../../http/lib/http"
require "../../logger/lib/logger"

# A small, fiber-based, Puma-like concurrent HTTP server for Diamond.
# Pulls in packages/http's own http_parse_request/http_write_response
# and packages/logger's own Logger (used by gremlin_worker to report an
# otherwise-silently-swallowed unhandled error from a request handler)
# by relative path -- `require_cut "http"`/`require_cut "logger"` is the
# mechanism for reaching an installed cut, but it's anchored to the
# process's own working directory and a fixed cuts/<name>/lib/<name>.di
# shape (see docs/packages.md), neither of which describes packages/http
# or packages/logger sitting two directories up in this repo's own
# tree; the relative path always resolves correctly regardless of where
# gremlin itself ends up.
# `require_cut "gremlin"` (once installed as a cut) then brings in
# gremlin_serve. Same Rack-style status/headers/body contract
# as http_serve, plus one addition http_serve has no equivalent for: a
# Callable[2] taking a request Hash *and* this worker's own persistent
# context Hash (empty on first use, otherwise whatever a previous
# request on this same worker left in it -- see "Per-worker context"
# below), returning [status, headers, body] -- but unlike http_serve's
# single blocking accept loop,
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
#
# ## Per-worker context
#
# `context` (gremlin_worker's own local, created once per worker before
# its accept loop starts) exists because `handler` can't just close over
# a Hash the ordinary way once threads > 1: Thread.new hard-rejects any
# Callable that captures local state ("Thread.new's callable must not
# capture any local state" -- it has no way to deep-copy arbitrary
# captured values across the heap boundary the way it copies `handler`
# itself, a plain zero-capture top-level function reference, see
# docs/threads.md). Diamond does now have class variables (`@@cvar`,
# per-VM storage -- see docs/object-model.md's "Class variables"), which
# could in principle serve the same per-worker-storage role (each
# Thread-spawned worker already has its own independent VM, hence its
# own independent `@@` slots) -- but `context` stays the simpler default
# here: it's an ordinary Hash any handler already receives with no setup,
# rather than requiring a handler to be wrapped in a class and given its
# own `@@` slot just to keep state. (`packages/rack`'s `RackChain` does
# use the `@@cvar` route, for a different reason: it's memoizing one
# specific computed value -- a composed middleware chain -- keyed by
# class, not threading arbitrary ad hoc state through every request the
# way `context` does.) `context` sidesteps the restriction by never
# crossing the Thread boundary in the first place: it's created by
# gremlin_worker itself,
# after Thread.new has already handed control to that worker's own
# thread, so it's an ordinary local captured by spawn_connection/
# handle_connection exactly like `conn` already is -- no Thread.new
# argument-crossing involved. Each worker's context is therefore its
# own: threads: N gives a handler N independent Hashes, one per worker,
# never shared or synchronized -- consistent with every other piece of
# per-worker state here (own heap, own listener, own connections list).

# One file per class (a handful here: NonblockingConnection is the only
# real class; gremlin_worker/gremlin_serve are plain top-level defs and
# share a file since neither stands alone). `nonblocking_connection`
# first: `server`'s own gremlin_worker references NonblockingConnection
# by name, and (per packages/arel/lib/arel.di's own comment) a
# class/module-name reference resolves regardless of require order via
# the compiler's declaration-discovery pass, but there's no reason to
# rely on that when the natural reading order already has the connection
# wrapper defined before the worker loop that uses it.
require "./gremlin/nonblocking_connection"
require "./gremlin/server"
