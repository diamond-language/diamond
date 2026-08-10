def unwrap(value: Int | Nil) -> Int
  unless value == nil
    return value
  else
    return 0
  end
end

puts(unwrap(nil))
unwrap(42)
