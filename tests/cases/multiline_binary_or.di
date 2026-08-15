def check(a, b)
  if a > 0 ||
     b > 0
    "at least one positive"
  else
    "neither positive"
  end
end
check(-1, 1)
