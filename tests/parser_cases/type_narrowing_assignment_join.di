def choose(flag: Bool) -> Int
  value = 1
  if flag
    value = 20
  else
    value = 22
  end
  return value
end

puts(choose(true))
choose(false)
