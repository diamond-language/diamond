# generators: fibers as generators, lazy pipelines, coroutines, and a
# cooperative scheduler.
#
#   diamond generators.di
require "./lib/generator"
require "./lib/scheduler"

# Prints a heading line for each part of the demo.
def section(title: String)
  puts("== #{title}")
end

# True when the number reads the same backwards (121, 1331...).
def palindrome?(n: Int) -> Bool
  text = n.to_s()
  text == text.reverse()
end

# --- Infinite sequences, consumed a few values at a time -----------------
section("infinite generators")

# `first(n)` takes just n values, so these generators never run to infinity.
puts("naturals:  #{naturals().first(8).join(" ")}")
puts("fibonacci: #{fibonacci().first(12).join(" ")}")
puts("primes:    #{primes().first(15).join(" ")}")
# Int promotes past 64 bits on its own, so the 150th Fibonacci is exact.
puts("fib(150):  #{fibonacci().first(151).last()}")

# --- Lazy pipelines: each stage is a generator pulling from the last -----
puts("")
section("lazy pipelines")

# Each stage wraps the one before it: squares of 1, 2, 3, ... filtered to
# palindromes. Nothing is computed until `first(7)` pulls the values through,
# one at a time, and it stops after seven.
squares = naturals(1).mapping() do |n| n * n end
puts("square palindromes: #{squares.selecting() do |n| palindrome?(n) end.first(7).join(" ")}")

# Palindromic primes above 10 (the one-digit primes are trivially
# palindromes, so they are excluded).
palindromic_primes = primes().selecting() do |p| palindrome?(p) && p > 10 end
puts("palindromic primes: #{palindromic_primes.first(6).join(" ")}")

# taking_while makes the sequence finite, so Enumerable's methods apply.
small_fibs = fibonacci().taking_while() do |n| n < 100 end
puts("fibonacci < 100:    #{small_fibs.to_a().join(" ")}")
puts("  sum #{small_fibs.sum()}, evens #{small_fibs.select() do |n| n % 2 == 0 end.join(",")}")
puts("  grouped by digits: #{small_fibs.group_by() do |n| n.to_s().length() end}")

# The built-in LazyEnumerator does the same for Arrays and Ranges: this
# would be a million-element array if it were eager, but only the numbers
# needed to find five matches are ever cubed.
cubes = (1..1_000_000).lazy().map() do |n| n * n * n end.select() do |n| n % 7 == 1 end
puts("cubes = 1 (mod 7):  #{cubes.take(5).join(" ")}")

# --- Coroutines: values flow both ways through resume/yield --------------
puts("")
section("coroutines")

# A coroutine passes values BOTH ways. `Fiber.yield(x)` hands x out to whoever
# called `resume`, and pauses; the value given to the NEXT `resume(v)`
# becomes what that `Fiber.yield` evaluates to. So this fiber receives a
# number with each resume and answers with the running average.
def make_averager()
  def averager()
    count = 0
    total = 0.0

    # The first yield sends nothing out; it just waits for the first sample.
    value = Fiber.yield(nil)

    loop do
      count += 1
      total += value
      value = Fiber.yield(total / count)
    end
  end
  averager
end

# Create the fiber and run it up to its first yield (nothing is sent in yet).
averager = Fiber.new(make_averager())
averager.resume()

# Then each resume sends a sample in and gets the average so far back.
[10, 20, 60, 30].each() do |sample|
  puts("sample #{sample} -> running average #{averager.resume(sample)}")
end
puts("status: #{averager.status()}")

# Another coroutine, used the other way: a generator that yields one word at
# a time as it scans the text, and keeps its place between resumes.
def make_tokenizer(text: String)
  def tokenizer()
    word = ""
    text.split("").each() do |char|
      if char == " "
        Fiber.yield(word) unless word.empty?()
        word = ""
      else
        word += char
      end
    end

    # The last word has no trailing space to trigger a yield. Then the
    # fiber's final value, :done, is what the last resume returns.
    Fiber.yield(word) unless word.empty?()
    :done
  end
  tokenizer
end

# Resume until the fiber returns its :done marker. (Note the double space in
# the text: empty words are skipped.)
words = Fiber.new(make_tokenizer("fibers  keep their place"))
loop do
  word = words.resume()
  break if word == :done
  puts("word: #{word}")
end

# A finished fiber cannot be resumed again.
puts("status: #{words.status()}, alive? #{words.alive?()}")
begin
  words.resume()
rescue error: FiberError
  puts("resuming again: FiberError (#{error.message()})")
end

# --- A cooperative scheduler over a simulated clock ----------------------
puts("")
section("scheduler")

# Four tasks sharing one thread. Each runs until it asks to wait
# (sleep_ticks / pass); the scheduler's simulated clock then decides who
# goes next, so the trace is identical on every run.
scheduler = Scheduler.new()

# Waits 3 ticks, returns "tea".
scheduler.spawn("kettle") do
  scheduler.log("kettle: heating")
  sleep_ticks(3)
  scheduler.log("kettle: boiled")
  "tea"
end

# Two slices, each taking 2 ticks.
scheduler.spawn("toaster") do
  2.times() do |slice|
    scheduler.log("toaster: slice #{slice + 1} in")
    sleep_ticks(2)
  end
  "toast"
end

# `pass()` yields for a single tick, so this interleaves with the others
# one step at a time.
scheduler.spawn("ticker") do
  count = 0
  while count < 4
    count += 1
    scheduler.log("ticker: #{count}")
    pass()
  end
  count
end

# A task that fails: the scheduler logs the error and carries on with the
# rest instead of crashing.
scheduler.spawn("smoke alarm") do
  sleep_ticks(2)
  raise RuntimeError.new("false alarm")
end

# Run everything to completion and print the trace.
scheduler.run().each() do |line| puts(line) end

exit(0)
