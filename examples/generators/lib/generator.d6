# A generator is a block that produces values with Fiber.yield. Each call
# to `each` or `first` runs the block in a fresh Fiber and resumes it once
# per value, so a generator can describe an infinite sequence and still be
# consumed a few values at a time.
class Generator
  include Enumerable

  # `&block` captures the body; it is run later, inside a Fiber, each time
  # the generator is consumed (so a generator can be consumed more than
  # once, each time from the start).
  def initialize(&block)
    @body = block
  end

  # Enumerable builds map/select/sort/sum/... on this. Those run the
  # generator to completion, so use them only on finite generators.
  def each(callback)
    fiber = Fiber.new(@body)

    # Resume until the body finishes. When the block ends, its final
    # (non-yielded) value comes back from `resume` too, but the fiber is no
    # longer alive, so that value is discarded rather than treated as an
    # element.
    loop do
      value = fiber.resume()
      break unless fiber.alive?()
      callback(value)
    end
    self
  end

  # The first n values; safe on an infinite generator.
  def first(n: Int) -> Array
    values = []
    return values if n <= 0
    fiber = Fiber.new(@body)

    # Stop after n values WITHOUT resuming again: this is what lets an
    # infinite generator be sampled.
    while values.length() < n
      value = fiber.resume()
      break unless fiber.alive?()
      values.push(value)
    end
    values
  end

  # Lazy counterparts of map/select/take_while: each returns a new
  # generator whose block pulls from this one. Nothing runs until a value
  # is asked for.
  #
  # Each keeps the source generator in a local (`source = self`) for the new
  # generator's block to pull from.
  def mapping(&transform) -> Generator
    source = self
    Generator.new() do
      source.each() do |value| Fiber.yield(transform(value)) end
    end
  end

  def selecting(&keep) -> Generator
    source = self
    Generator.new() do
      source.each() do |value|
        Fiber.yield(value) if keep(value)
      end
    end
  end

  # Unlike the other two this one drives the source's fiber by hand, because
  # it must STOP early, whereas `each` always runs the source to completion.
  # It reads the source's body through the protected
  # `body`, which is allowed because source is also a Generator.
  def taking_while(&keep) -> Generator
    source = self
    Generator.new() do
      fiber = Fiber.new(source.body())

      loop do
        value = fiber.resume()
        break unless fiber.alive?() && keep(value)
        Fiber.yield(value)
      end
    end
  end

  protected

  def body() = @body
end

# 0, 1, 2, ... (or from any start).
def naturals(from: Int = 0) -> Generator
  Generator.new() do
    n = from
    loop do
      Fiber.yield(n)
      n += 1
    end
  end
end

# 0, 1, 1, 2, 3, 5, ... Parallel assignment updates both values at once, so
# the old `a` is still available for the sum.
def fibonacci() -> Generator
  Generator.new() do
    a, b = 0, 1

    loop do
      Fiber.yield(a)
      a, b = b, a + b
    end
  end
end

# An incremental Sieve of Eratosthenes that needs no upper bound:
# `composites` maps each upcoming composite number to the primes that
# produce it. Reaching a number in the table means it's composite -- its
# entry moves on to each prime's next multiple and is deleted, so the table
# only ever holds about one entry per prime found so far.
def primes() -> Generator
  Generator.new() do
    composites = {}
    n = 2

    loop do
      factors = composites.delete(n)

      # Not in the table: n is prime. Yield it, and record that its first
      # composite worth marking is n*n (smaller multiples were already
      # marked by smaller primes).
      if factors == nil
        Fiber.yield(n)
        composites[n * n] = [n]
      # In the table: n is composite. Move each prime that "hit" it on to its
      # next multiple (n + p). Several primes can land on one number, so
      # the entry is a list.
      else
        factors.each() do |p|
          composites[n + p] = composites.fetch(n + p, []).push(p)
        end
      end
      n += 1
    end
  end
end
