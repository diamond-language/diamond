def make_offset(offset)
  def apply(value) = offset + value
  apply
end

offset = make_offset(10)
puts(offset(7))
puts(offset(9))
