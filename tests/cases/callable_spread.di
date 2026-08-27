def multiply(left, right)
  left * right
end

callable = multiply
puts(callable(*[6, 7]))

def collect(prefix, *values)
  "#{prefix}:#{values.join(",")}"
end

collector = collect
puts(collector(*["values", 1, 2, 3]))

class Factory
  def initialize(offset)
    @offset = offset
  end

  def build()
    closure adder(left, right)
      @offset + left + right
    end
    adder
  end
end
adder = Factory.new(5).build()
puts(adder(*[10, 20]))
nil
