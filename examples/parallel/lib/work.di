# The work the demo spreads across threads. Every function here is a plain
# top-level def that captures nothing, which is what Thread.new and
# Supervisor#add_child require: each thread runs it in its own VM and heap.

# The start below `high` (and at or above `low`) with the longest Collatz
# sequence, as [start, length].
#
# The Collatz rule: halve an even number, otherwise triple it and add 1,
# until reaching 1. It is pure CPU work with no I/O, so it shows what extra
# cores buy.
def collatz_longest(low: Int, high: Int) -> Array
  best = low
  best_length = 0
  n = low

  while n < high
    # Count the steps for this starting value (the length includes the
    # starting number itself).
    x = n
    length = 1
    while x != 1
      x = if x % 2 == 0 then x / 2 else 3 * x + 1 end
      length += 1
    end

    # Keep the longest so far; on a tie the smaller start wins (strict >).
    if length > best_length
      best = n
      best_length = length
    end
    n += 1
  end
  [best, best_length]
end

# Word counts for one document, lowercased, punctuation dropped.
def count_words(text: String) -> Hash
  # Replace everything but letters, apostrophes and spaces with a space,
  # split into words, drop empties, and `tally` counts each distinct word.
  words = text.downcase().gsub(Regexp.new("[^a-z' ]"), " ").split(" ")
  words.reject() do |word| word.empty?() end.tally()
end

# --- Worker-pool pieces --------------------------------------------------

# Sends each job, then closes the channel so the workers know to stop.
# The producer. Each job is [its position, the text], so results can be put
# back in order later.
def feed(jobs: Channel, documents: Array)
  documents.each_with_index() do |text, index|
    jobs.send([index, text])
  end
  jobs.close()
end

# Takes jobs until the channel is closed and drained, sends back one result
# per job, and returns how many it handled.
def word_worker(jobs: Channel, results: Channel) -> Int
  handled = 0
  loop do
    job = jobs.receive()
    break if job == nil
    [index, text] = job
    results.send([index, count_words(text)])
    handled += 1
  end
  handled
end

# --- Pipeline stages: each reads one channel and writes the next ---------

# Three stages connected by channels, each running in its own thread:
# numbers -> squares -> [n, digit sum]. Each closes its output when its input
# ends, which cascades the "no more data" signal down the pipeline.
def stage_numbers(output: Channel, limit: Int)
  1.upto(limit) do |n| output.send(n) end
  output.close()
end

def stage_squares(input: Channel, output: Channel)
  loop do
    n = input.receive()
    break if n == nil
    output.send(n * n)
  end
  output.close()
end

def stage_digit_sums(input: Channel, output: Channel)
  loop do
    n = input.receive()
    break if n == nil
    sum = n.to_s().split("").map() do |digit| digit.to_i() end.sum()
    output.send([n, sum])
  end
  output.close()
end

# --- Isolation and failure -----------------------------------------------

# A counter in a class variable. Each thread has its OWN copy of the
# module's state, which is what the isolation demo shows.
module Tally
  def self.bump() -> Int
    @@count = (@@count || 0) + 1
  end
end

# Mutates its own copy of the array; the caller's is untouched.
def append_and_bump(values: Array) -> Array
  values.push("added by worker")
  [values, Tally.bump()]
end

# Fails for negative input, to show an error in a thread surfacing at `join`.
def fail_on(value: Int)
  raise ArgumentError.new("worker rejected #{value}") if value < 0
  value
end

# Crashes on its first two attempts. Each attempt starts in a fresh heap, so
# it learns which attempt it is from a ticket channel the parent filled.
def flaky_worker(tickets: Channel, done: Channel)
  attempt = tickets.receive()
  raise RuntimeError.new("flaked on attempt #{attempt}") if attempt < 3
  done.send("succeeded on attempt #{attempt}")
end
