def is_even(x)
  x - (x / 2) * 2 == 0
end
def double(x)
  x * 2
end
def add(acc, x)
  acc + x
end
selected = (1..6).select(is_even)
mapped = (1..3).map(double)
reduced = (1..5).reduce(0, add)
"#{selected}, #{mapped}, #{reduced}"
