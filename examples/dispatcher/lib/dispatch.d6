# The work the dispatcher spreads across threads. Like examples/parallel, every
# function here is a plain top-level def that captures nothing: Thread.new and
# Supervisor#add_child run it in its own VM and heap.

# Takes jobs from whichever of `primary` and `overflow` has one, until a
# message arrives on `cancel`. Listing `cancel` first makes it win whenever
# several channels are ready at once, so a cancellation is never starved by a
# busy queue.
def drain(cancel: Channel, primary: Channel, overflow: Channel, done: Channel)
  loop
    picked = Channel.select([cancel, primary, overflow])
    break if picked == nil

    # picked is [index, value]: which channel in the list, and what it held.
    index = picked[0]
    break if index == 0

    done.send(["primary", "overflow"][index - 1] + ":" + picked[1].to_s())
  end
  "drained"
end

# A worker that always crashes, so a restart policy has something to give up on.
def always_crashes()
  raise "no luck"
end
