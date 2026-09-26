# The work the demo spreads across threads. Every function here is a plain
# top-level def that captures nothing, which is what Thread.new and
# Supervisor#add_child require: each thread runs it in its own VM and heap.

# The start below `high` (and at or above `low`) with the longest Collatz
# sequence, as [start, length].
def collatz_longest(low: Int, high: Int) -> Array
  best = low
  best_length = 0
  n = low
  while n < high
    x = n
    length = 1
    while x != 1
      x = if x % 2 == 0 then x / 2 else 3 * x + 1 end
      length += 1
    end
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
  words = text.downcase().gsub(Regexp.new("[^a-z' ]"), " ").split(" ")
  words.reject() do |word| word.empty?() end.tally()
end

# --- Worker-pool pieces --------------------------------------------------

# Sends each job, then closes the channel so the workers know to stop.
def feed(jobs, documents: Array)
  documents.each_with_index() do |text, index|
    jobs.send([index, text])
  end
  jobs.close()
end

# Takes jobs until the channel is closed and drained, sends back one result
# per job, and returns how many it handled.
def word_worker(jobs, results) -> Int
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

def stage_numbers(output, limit: Int)
  1.upto(limit) do |n| output.send(n) end
  output.close()
end

def stage_squares(input, output)
  loop do
    n = input.receive()
    break if n == nil
    output.send(n * n)
  end
  output.close()
end

def stage_digit_sums(input, output)
  loop do
    n = input.receive()
    break if n == nil
    sum = n.to_s().split("").map() do |digit| digit.to_i() end.sum()
    output.send([n, sum])
  end
  output.close()
end

# --- Isolation and failure -----------------------------------------------

class Tally
  def self.bump() -> Int
    @@count = (@@count || 0) + 1
  end
end

# Mutates its own copy of the array; the caller's is untouched.
def append_and_bump(values: Array) -> Array
  values.push("added by worker")
  [values, Tally.bump()]
end

def fail_on(value: Int)
  raise ArgumentError.new("worker rejected #{value}") if value < 0
  value
end

# Crashes on its first two attempts. Each attempt starts in a fresh heap, so
# it learns which attempt it is from a ticket channel the parent filled.
def flaky_worker(tickets, done)
  attempt = tickets.receive()
  raise RuntimeError.new("flaked on attempt #{attempt}") if attempt < 3
  done.send("succeeded on attempt #{attempt}")
end
