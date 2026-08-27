class ContextBuilder
  def initialize(prefix, &block: Callable[[String], String])
    @value = yield(prefix)
  end

  def value() -> String
    @value
  end
end

class InheritedContextBuilder < ContextBuilder
end

fixed = ContextBuilder.new("fixed") do |value|
  value + " constructor"
end
puts(fixed.value())

spread = InheritedContextBuilder.new(*["spread"]) do |value|
  value + " constructor"
end
puts(spread.value())

keyword = ContextBuilder.new(prefix: "keyword") do |value|
  value + " constructor"
end
puts(keyword.value())
