# parallel: OS threads, channels, and supervisors.
#
#   diamond parallel.di           # deterministic output
#   diamond parallel.di --time    # also report serial vs. parallel timings
#
# Every thread runs in its own VM with its own heap. Arguments and results
# are deep-copied across the boundary; a Channel is the one value that
# crosses by reference.
require "./lib/work"

# --time adds wall-clock measurements, which vary run to run, so they go to
# stderr and only when asked. Everything on stdout is deterministic.
timing = ARGV.include?("--time")

# --- 1. Fan out with Thread.new, gather with join ------------------------
puts("== fan-out")
# Split 1..24000 into 4 equal ranges, one thread each. `Thread.new(fn, args...)`
# runs a function in its own VM; the arguments are copied in.
limit = 24_000
workers = 4
step = limit / workers
threads = (0...workers).map() do |i|
  Thread.new(collatz_longest, 1 + i * step, 1 + (i + 1) * step)
end

# join waits for a thread and returns its result (copied back).
partials = threads.map() do |thread| thread.join() end
partials.each_with_index() do |partial, i|
  puts("  range #{i + 1}: #{partial[0]} runs for #{partial[1]} steps")
end

# The overall answer is the best of the four partial answers.
[start, steps] = partials.max_by() do |partial| partial[1] end
puts("longest Collatz sequence below #{limit + 1}: #{start}, #{steps} steps")

# Optional timing: run the same work on one thread, then again on four, and
# compare. Also confirms both give the same answer.
if timing
  began = Time.monotonic()
  serial = collatz_longest(1, limit + 1)
  serial_seconds = Time.monotonic() - began
  began = Time.monotonic()
  (0...workers).map() do |i|
    Thread.new(collatz_longest, 1 + i * step, 1 + (i + 1) * step)
  end.each() do |thread| thread.join() end
  parallel_seconds = Time.monotonic() - began
  warn("  serial #{"%.2f".format(serial_seconds)}s, #{workers} threads #{"%.2f".format(parallel_seconds)}s (#{"%.1f".format(serial_seconds / parallel_seconds)}x)")
  warn("  same answer serially: #{serial == [start, steps]}")
end

# --- 2. A worker pool fed through channels -------------------------------
puts("")
puts("== worker pool")
documents = [
  "It was the best of times, it was the worst of times.",
  "Call me Ishmael. Some years ago, never mind how long precisely.",
  "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife.",
  "All happy families are alike; each unhappy family is unhappy in its own way.",
  "The sky above the port was the color of television, tuned to a dead channel.",
  "In a hole in the ground there lived a hobbit.",
  "It was a bright cold day in April, and the clocks were striking thirteen.",
  "Happy families are all alike, and it was the age of wisdom, it was the age of foolishness.",
]

# Two bounded channels (capacity 4): one carries documents to the workers,
# one carries counts back. A full channel makes `send` wait, so the feeder
# cannot run far ahead of the workers. Channels are the one thing passed by
# reference rather than copied.
jobs = Channel.new(4)
results = Channel.new(4)
feeder = Thread.new(feed, jobs, documents)
pool = (1..3).map() do |_| Thread.new(word_worker, jobs, results) end

# Results arrive in whatever order the workers finish; merge them by key.
totals = {}
per_document = []
documents.length().times() do |_|
  [index, counts] = results.receive()
  per_document.push([index, counts.values().sum()])
  counts.each() do |word, count|
    totals[word] = totals.fetch(word, 0) + count
  end
end

# Wait for the feeder and workers. Each worker returns how many jobs it
# took; the total must equal the number of documents.
feeder.join()
handled = pool.map() do |worker| worker.join() end
puts("#{documents.length()} documents, #{handled.sum()} handled by #{pool.length()} workers")
per_document.sort_by() do |pair| pair[0] end.each() do |pair|
  puts("  document #{pair[0] + 1}: #{pair[1]} words")
end

# Most frequent first, ties alphabetical: Arrays compare element by element.
top = totals.keys().sort_by() do |word| [0 - totals[word], word] end.take(6)
puts("most common: #{top.map() do |word| "#{word} (#{totals[word]})" end.join(", ")}")

# --- 3. A pipeline: one thread per stage ---------------------------------
puts("")
puts("== pipeline")

# Three threads connected by small channels; data flows through all of them
# at once.
numbers = Channel.new(2)
squares = Channel.new(2)
sums = Channel.new(2)
stages = [
  Thread.new(stage_numbers, numbers, 12),
  Thread.new(stage_squares, numbers, squares),
  Thread.new(stage_digit_sums, squares, sums),
]

# The main thread reads the last channel until it is closed (nil).
row = []
loop do
  pair = sums.receive()
  break if pair == nil
  row.push("#{pair[0]}:#{pair[1]}")
end

stages.each() do |stage| stage.join() end
puts("square:digit-sum #{row.join(" ")}")
puts("channel closed? #{sums.closed?()}, size #{sums.size()}")

# `try_receive` never waits: on an empty channel it raises WouldBlockError.
begin
  Channel.new(1).try_receive()
rescue error: WouldBlockError
  puts("try_receive on an empty channel: WouldBlockError")
end

# --- 4. Isolation ---------------------------------------------------------
puts("")
puts("== isolation")
# The worker gets a COPY of the array and its own Tally, so changes on
# either side are invisible to the other.
mine = ["mine"]
Tally.bump()
Tally.bump()
[theirs, their_count] = Thread.new(append_and_bump, mine).join()
puts("parent array: #{mine}, worker's copy: #{theirs}")
puts("parent Tally count: #{Tally.bump()}, worker's: #{their_count}")

# Thread.new only accepts functions that capture nothing (see lib/work.di),
# so a nested function that captures a local is refused with a TypeError.
def make_capturing()
  secret = 42
  def peek() = secret
  peek
end
begin
  Thread.new(make_capturing())
rescue error: TypeError
  puts("capturing closure: TypeError")
end

# --- 5. Failure: join re-raises, a Supervisor restarts --------------------
puts("")
puts("== failure")
# An exception in a thread is re-raised in whoever calls `join`.
failing = Thread.new(fail_on, -1)
begin
  failing.join()
rescue error: ArgumentError
  puts("join re-raised: #{error.message()}")
end

# A supervised worker that fails twice. A restarted worker starts with a
# fresh heap and no memory of earlier attempts, so the parent preloads a
# ticket per attempt on a channel (the channel survives restarts) and the
# worker reads which attempt it is.
tickets = Channel.new(3)
[1, 2, 3].each() do |ticket| tickets.send(ticket) end
done = Channel.new(1)
supervisor = Supervisor.new()
child = supervisor.add_child(flaky_worker, tickets, done)
puts(done.receive())
supervisor.join()

# Two crashes, then success: two restarts, and the last error recorded.
puts("restarts: #{supervisor.restart_count(child)}, last error: #{supervisor.last_error(child)}")

exit(0)
