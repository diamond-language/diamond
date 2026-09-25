# generators: fibers as generators, lazy pipelines, coroutines, and a
# cooperative scheduler.
#
#   diamond generators.di
require "./lib/generator"
require "./lib/scheduler"

def section(title: String)
  puts("== #{title}")
end

def palindrome?(n: Int) -> Bool
  text = n.to_s()
  text == text.reverse()
end

# --- Infinite sequences, consumed a few values at a time -----------------
section("infinite generators")
puts("naturals:  #{naturals().first(8).join(" ")}")
puts("fibonacci: #{fibonacci().first(12).join(" ")}")
puts("primes:    #{primes().first(15).join(" ")}")
# Int promotes past 64 bits on its own, so the 150th Fibonacci is exact.
puts("fib(150):  #{fibonacci().first(151).last()}")

# --- Lazy pipelines: each stage is a generator pulling from the last -----
puts("")
section("lazy pipelines")
squares = naturals(1).mapping() do |n| n * n end
puts("square palindromes: #{squares.selecting() do |n| palindrome?(n) end.first(7).join(" ")}")
palindromic_primes = primes().selecting() do |p| palindrome?(p) && p > 10 end
puts("palindromic primes: #{palindromic_primes.first(6).join(" ")}")
small_fibs = fibonacci().taking_while() do |n| n < 100 end
# taking_while makes the sequence finite, so Enumerable's methods apply.
puts("fibonacci < 100:    #{small_fibs.to_a().join(" ")}")
puts("  sum #{small_fibs.sum()}, evens #{small_fibs.select() do |n| n % 2 == 0 end.join(",")}")
puts("  grouped by digits: #{small_fibs.group_by() do |n| n.to_s().length() end}")
# The built-in LazyEnumerator does the same for Arrays and Ranges.
cubes = (1..1_000_000).lazy().map() do |n| n * n * n end.select() do |n| n % 7 == 1 end
puts("cubes = 1 (mod 7):  #{cubes.take(5).join(" ")}")

# --- Coroutines: values flow both ways through resume/yield --------------
puts("")
section("coroutines")
def make_averager()
  def averager()
    count = 0
    total = 0.0
    value = Fiber.yield(nil)
    loop do
      count += 1
      total += value
      value = Fiber.yield(total / count)
    end
  end
  averager
end
averager = Fiber.new(make_averager())
averager.resume()   # run to the first yield
[10, 20, 60, 30].each() do |sample|
  puts("sample #{sample} -> running average #{averager.resume(sample)}")
end
puts("status: #{averager.status()}")

def make_tokenizer(text: String)
  def tokenizer()
    word = ""
    text.split("").each() do |char|
      if char == " "
        Fiber.yield(word) if word.length() > 0
        word = ""
      else
        word += char
      end
    end
    Fiber.yield(word) if word.length() > 0
    :done
  end
  tokenizer
end
words = Fiber.new(make_tokenizer("fibers  keep their place"))
loop do
  word = words.resume()
  break if word == :done
  puts("word: #{word}")
end
puts("status: #{words.status()}, alive? #{words.alive?()}")
begin
  words.resume()
rescue error: FiberError
  puts("resuming again: FiberError (#{error.message()})")
end

# --- A cooperative scheduler over a simulated clock ----------------------
puts("")
section("scheduler")
scheduler = Scheduler.new()
scheduler.spawn("kettle") do
  scheduler.log("kettle: heating")
  sleep_ticks(3)
  scheduler.log("kettle: boiled")
  "tea"
end
scheduler.spawn("toaster") do
  2.times() do |slice|
    scheduler.log("toaster: slice #{slice + 1} in")
    sleep_ticks(2)
  end
  "toast"
end
scheduler.spawn("ticker") do
  count = 0
  while count < 4
    count += 1
    scheduler.log("ticker: #{count}")
    pass()
  end
  count
end
scheduler.spawn("smoke alarm") do
  sleep_ticks(2)
  raise RuntimeError.new("false alarm")
end
scheduler.run().each() do |line| puts(line) end

exit(0)
