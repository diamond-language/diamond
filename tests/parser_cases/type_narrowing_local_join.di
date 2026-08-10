def choose(flag: Bool) -> Int
  value = true
  if flag
    value = 20
  else
    value = 22
  end
  value
end

choose(false) == 22
