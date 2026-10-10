class ReaderValue
end

interface ReaderContract
  def value() -> ReaderValue
end

class ReaderBase
  attr_reader value
end

class ReaderHolder < ReaderBase
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
