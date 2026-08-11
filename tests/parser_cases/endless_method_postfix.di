class Selector
  def initialize(value)
    @value = value
  end

  def selected(flag) = @value if flag
  def fallback(flag) = @value unless flag
end

selector = Selector.new(23)
puts(selector.selected(true))
puts(selector.selected(false))
puts(selector.fallback(false))
puts(selector.fallback(true))
