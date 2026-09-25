# parallel: OS threads, channels, and supervisors.
#
#   diamond parallel.di           # deterministic output
#   diamond parallel.di --time    # also report serial vs. parallel timings
#
# Every thread runs in its own VM with its own heap. Arguments and results
# are deep-copied across the boundary; a Channel is the one value that
# crosses by reference.
require "./lib/work"

timing = ARGV.include?("--time")

# --- 1. Fan out with Thread.new, gather with join ------------------------
puts("== fan-out")
limit = 24_000
workers = 4
step = limit / workers
threads = (0...workers).map() do |i|
  Thread.new(collatz_longest, 1 + i * step, 1 + (i + 1) * step)
end
partials = threads.map() do |thread| thread.join() end
partials.each_with_index() do |partial, i|
  puts("  range #{i + 1}: #{partial[0]} runs for #{partial[1]} steps")
end
[start, steps] = partials.max_by() do |partial| partial[1] end
puts("longest Collatz sequence below #{limit + 1}: #{start}, #{steps} steps")
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
feeder.join()
handled = pool.map() do |worker| worker.join() end
puts("#{documents.length()} documents, #{handled.sum()} handled by #{pool.length()} workers")
per_document.sort_by() do |pair| pair[0] end.each() do |pair|
  puts("  document #{pair[0] + 1}: #{pair[1]} words")
end
# Most frequent first, ties alphabetical. Arrays aren't ordered, so the sort
# key is one String: the count's distance below 9999, padded, then the word.
top = totals.keys().sort_by() do |word| "%04d %s".format([9999 - totals[word], word]) end.take(6)
puts("most common: #{top.map() do |word| "#{word} (#{totals[word]})" end.join(", ")}")

# --- 3. A pipeline: one thread per stage ---------------------------------
puts("")
puts("== pipeline")
numbers = Channel.new(2)
squares = Channel.new(2)
sums = Channel.new(2)
stages = [
  Thread.new(stage_numbers, numbers, 12),
  Thread.new(stage_squares, numbers, squares),
  Thread.new(stage_digit_sums, squares, sums),
]
row = []
loop do
  pair = sums.receive()
  break if pair == nil
  row.push("#{pair[0]}:#{pair[1]}")
end
stages.each() do |stage| stage.join() end
puts("square:digit-sum #{row.join(" ")}")
puts("channel closed? #{sums.closed?()}, size #{sums.size()}")
begin
  Channel.new(1).try_receive()
rescue error: WouldBlockError
  puts("try_receive on an empty channel: WouldBlockError")
end

# --- 4. Isolation ---------------------------------------------------------
puts("")
puts("== isolation")
mine = ["mine"]
Tally.bump()
Tally.bump()
[theirs, their_count] = Thread.new(append_and_bump, mine).join()
puts("parent array: #{mine}, worker's copy: #{theirs}")
puts("parent Tally count: #{Tally.bump()}, worker's: #{their_count}")
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
failing = Thread.new(fail_on, -1)
begin
  failing.join()
rescue error: ArgumentError
  puts("join re-raised: #{error.message()}")
end

tickets = Channel.new(3)
[1, 2, 3].each() do |ticket| tickets.send(ticket) end
done = Channel.new(1)
supervisor = Supervisor.new()
child = supervisor.add_child(flaky_worker, tickets, done)
puts(done.receive())
supervisor.join()
puts("restarts: #{supervisor.restart_count(child)}, last error: #{supervisor.last_error(child)}")

exit(0)
