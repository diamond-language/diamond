def make_offset(offset)
  def apply(value, enabled) = offset + value if enabled
  apply
end

offset = make_offset(10)
puts(offset(8, true))
puts(offset(8, false))
puts(offset(11, true))
