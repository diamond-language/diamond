def describe(x: Int | String)
  case x
  when 0
    "zero"
  end
end
puts(describe(0))
puts(describe(5))
