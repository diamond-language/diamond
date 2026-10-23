class FactoryValue
end

interface FactoryContract
  def build() -> FactoryValue
end

def build_value() = FactoryValue.new()
def wrap_value() = build_value()
def wrap_early(flag)
  if flag then return wrap_value() end
  build_value()
end

class WrapperFactory
  def build() = wrap_value()
end

def needs_int(value: Int) = value

puts(begin
  needs_int(wrap_value())
rescue error: TypeError
  "wrapper checked at runtime"
end)
puts(begin
  needs_int(wrap_early(true))
rescue error: TypeError
  "early return checked at runtime"
end)
WrapperFactory.new() is FactoryContract
