# Dataloader-style N+1 batching. `context["dataloader"]` (set once per
# request by execution/executor.di) is a `Dataloader`; a resolver calls
# `context["dataloader"].with("some_name", BatchModule.batch).load(key)`
# to have its own request coalesced with every other `.load()` call
# against the same name within the same LOCAL group of siblings being
# resolved together (a list's own items, or a selection set's own
# fields -- see execution/executor.di's own #execute_selection_set/
# #complete_list_value).
#
# Deliberately LOCAL batching, not graphql-ruby's own GLOBAL
# coalescing across the entire query tree -- confirmed and designed
# around directly (not assumed) before writing this: Diamond's `Fiber`
# is a bare resume/yield/alive? primitive with no scheduler, no
# promise/future type, no fiber pool, so achieving fully general
# cross-branch coalescing would mean every "wait for my children" point
# in the executor bubbling its own "still stuck" state up through
# however many levels of Fiber nesting sit above it -- a
# project-sized undertaking on its own. What ships here still solves
# the dominant real-world N+1 shape (N parents, each independently
# resolving the same child field/association) since #complete_list_value
# spawns one fiber per list item and drives them as one group.
#
# No value-passing through yield/.resume() anywhere in this file --
# `Loader#load` waits by looping a bare `yield` until its own `@cache`
# (an ordinary Hash, closed over normally, no fiber value-handoff
# needed) has the key; the caller's own fiber body is responsible for
# writing its eventual result into a shared output container it closed
# over. Two properties this design leans on were already confirmed
# directly, empirically, before writing this file: bare `yield` from
# several call-frames deep inside a fiber's body suspends correctly
# (not just from the fiber's own literal top-level code -- matches
# packages/gremlin/lib/gremlin/nonblocking_connection.di's own
# #fill_more, which does exactly this), and an exception raised inside
# a fiber's body propagates out through `.resume()` to the resumer
# (matches packages/gremlin/lib/gremlin/server.di's own
# `spawn_connection`, which already rescues around a `.resume()` call).
module GraphQL
module Execution

class Dataloader
  def initialize()
    @loaders = {}
  end

  # First call for `name` builds and caches a Loader wrapping
  # `batch_fn`; every later call with the same `name` (from anywhere in
  # the request, since this Dataloader instance is shared for the whole
  # request via context["dataloader"]) returns that same instance,
  # ignoring its own `batch_fn` argument. Every call site for the "same"
  # loader name is expected to pass an equivalent batch_fn -- see
  # README.md's own "Dataloader" section.
  def with(name, batch_fn)
    if @loaders.include_key?(name)
      @loaders[name]
    else
      loader = Loader.new(batch_fn, self)
      @loaders[name] = loader
      loader
    end
  end

  # Drives `fibers` (already-constructed, not yet resumed -- one per
  # item/field in a single local group, see this file's own header
  # comment) to completion: resume every still-alive one once, then --
  # once none of them can make further progress on their own -- dispatch
  # every Loader with pending keys and resume the group again, repeating
  # until it's done. Each fiber body writes its own result into whatever
  # shared output container its own caller closed over; #run itself
  # returns nothing, callers read their own output container back after
  # this returns.
  def run(fibers, context)
    still_running = fibers
    while still_running.length() > 0
      next_round = []
      index = 0
      while index < still_running.length()
        fiber = still_running[index]
        fiber.resume()
        if fiber.alive?()
          next_round.push(fiber)
        end
        index += 1
      end
      still_running = next_round
      if still_running.length() > 0
        self.dispatch_pending(context)
      end
    end
  end

  def dispatch_pending(context)
    names = @loaders.keys()
    index = 0
    dispatched_any = false
    while index < names.length()
      loader = @loaders[names[index]]
      if loader.pending?()
        loader.dispatch(context)
        dispatched_any = true
      end
      index += 1
    end
    # Every fiber that's still alive at this point yielded from inside
    # Loader#load, which always registers its own key as pending before
    # yielding -- so if nothing had anything pending, either this file
    # has a bug or something yielded for a reason other than
    # Loader#load, neither of which should ever happen in practice.
    unless dispatched_any
      raise GraphQL::ExecutionError.new(
        "dataloader stalled: pending fibers but no loader has pending keys")
    end
  end
end

class Loader
  def initialize(batch_fn, dataloader)
    @batch_fn = batch_fn
    @dataloader = dataloader
    @cache = {}
    @failed = {}
    @pending = []
  end

  def load(key)
    if @cache.include_key?(key)
      return @cache[key]
    end
    if @failed.include_key?(key)
      raise GraphQL::ExecutionError.new(@failed[key])
    end
    unless @pending.include?(key)
      @pending.push(key)
    end
    while !@cache.include_key?(key) && !@failed.include_key?(key)
      yield
    end
    if @failed.include_key?(key)
      raise GraphQL::ExecutionError.new(@failed[key])
    end
    @cache[key]
  end

  def pending?() = @pending.length() > 0

  # A batch_fn failure must not hang every fiber waiting on this
  # loader forever (they'd keep looping on `yield` since @cache would
  # never fill for their key) or crash the whole request uncaught (this
  # runs outside any single fiber's own begin/rescue -- called directly
  # by Dataloader#run, not from inside a fiber body) -- every key in the
  # failed batch gets marked so #load raises a normal, per-field-
  # catchable GraphQL::ExecutionError the next time each waiting fiber
  # is resumed, exactly like any other resolver exception already is.
  def dispatch(context)
    keys = @pending
    @pending = []
    begin
      results = @batch_fn(keys, context)
      index = 0
      while index < keys.length()
        @cache[keys[index]] = results[keys[index]]
        index += 1
      end
    rescue e: StandardError
      index = 0
      while index < keys.length()
        @failed[keys[index]] = e.message()
        index += 1
      end
    end
  end
end

end
end
