class Box
  attr_accessor amount
  def initialize(v)
    @amount = v
  end
end

b = Box.new(1)
message = nil
begin
  b.amount + 5
rescue error: TypeError
  message = error.message()
end

# A genuine type mismatch with no Callable involved must NOT get the
# hint appended -- only the Callable-specific message shape changes.
plain_message = nil
begin
  5 + "not a number"
rescue error: TypeError
  plain_message = error.message()
end

[message.include?("method reference"), plain_message.include?("method reference")]
