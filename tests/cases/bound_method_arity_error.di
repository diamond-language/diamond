class BoundArityError
  def offset(value: Int, amount: Int = 0) -> Int = value + amount
end

offset = BoundArityError.new().offset
offset()
