class ReaderValue
end

interface ReaderContract
  def value() -> ReaderValue
end

class ReaderHolder
  attr_reader value
  def initialize()
    @value = ReaderValue.new()
  end
end

def needs_int(value: Int)
  value
end

checked = begin
  needs_int(ReaderHolder.new().value())
rescue error: TypeError
  "checked at runtime"
end

puts(checked)
ReaderHolder.new() is ReaderContract
