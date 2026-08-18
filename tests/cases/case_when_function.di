def classify(n)
  case n
  when 0
    "zero"
  when 1, 2, 3
    "small"
  else
    "big"
  end
end
"#{classify(0)}, #{classify(2)}, #{classify(100)}"
