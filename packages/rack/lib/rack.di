# A small, server-agnostic Rack-style middleware layer for Diamond.
# Composes with packages/http's http_serve and packages/gremlin's
# gremlin_serve (see this package's README for both), but this file
# itself has no dependency on either -- no Fiber, no Thread, no socket
# types, just the request Hash / [status, headers, body] response
# convention both servers already share.
#
# A middleware is a Callable[3]: (request, context, forward), where
# `forward` is a Callable[2]: (request, context) -> response, standing
# for "the rest of the chain." Calling forward(request, context)
# continues on; not calling it short-circuits (an auth middleware
# returning [401, ..., ...] directly, say). The terminal application
# handler stays an ordinary Callable[2] -- the exact shape gremlin_serve
# and (with one extra argument dropped, see README) http_serve already
# want -- so an existing handler needs no changes to become the tail of
# a chain.
#
# ## Why this isn't just nested closures
#
# The obvious Rack implementation -- `middleware(app)` returns a closure
# capturing `app` -- runs straight into a real constraint:
# gremlin_serve(..., threads: N) spawns each worker via Thread.new, and
# Thread.new categorically rejects any Callable that captures local
# state (see docs/threads.md -- a captured DiamondCell is live GC state
# tied to one heap, and Thread heaps are fully isolated). A naively
# composed middleware chain, being a capturing closure, could never be
# handed to gremlin_serve as `handler` once threads > 1.
#
# rack_compose sidesteps this by returning a plain Array (data, not a
# closure) -- the chain itself is just as crossable as any other
# Array/Hash argument. The one closure this file does build
# (rack_run_chain's own `forward`) is built fresh on each call, entirely
# inside whichever heap is already running the request -- it never
# crosses a Thread.new boundary, so capturing there is completely fine.
#
# RackChain (below) goes one step further for the threads > 1 case:
# memoizing the composed chain in a class variable means each spawned
# worker -- already its own fully independent DiamondVm/heap, per
# Thread's isolated-heap design -- builds its own copy the first time it
# handles a request, and the only thing that ever actually crosses
# Thread.new is an ordinary zero-capture top-level `def` (the app's own
# `rack_app`-style entry point below), exactly what gremlin_serve's
# `handler` contract already requires today, unchanged.

# One file per logical grouping: the plain chain-building/running
# functions (chain_composition), then RackChain, the one real class
# here (rack_chain). Neither references the other, so order between
# them doesn't matter -- kept in the same order as the original
# single-file layout.
require "./rack/chain_composition"
require "./rack/rack_chain"
