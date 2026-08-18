def classify(x)
  x == 1 ? "one" : x == 2 ? "two" : "other"
end
"#{classify(1)}, #{classify(2)}, #{classify(3)}"
