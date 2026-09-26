# A generator is a block that produces values with Fiber.yield. Each call
# to `each` or `first` runs the block in a fresh Fiber and resumes it once
# per value, so a generator can describe an infinite sequence and still be
# consumed a few values at a time.
class Generator
  include Enumerable

  def initialize(&block)
    @body = block
  end

  # Enumerable builds map/select/sort/sum/... on this. Those run the
  # generator to completion, so use them only on finite generators.
  def each(callback)
    fiber = Fiber.new(@body)
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

def naturals(from: Int = 0) -> Generator
  Generator.new() do
    n = from
    loop do
      Fiber.yield(n)
      n += 1
    end
  end
end

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
      if factors == nil
        Fiber.yield(n)
        composites[n * n] = [n]
      else
        factors.each() do |p|
          composites[n + p] = composites.fetch(n + p, []).push(p)
        end
      end
      n += 1
    end
  end
end
