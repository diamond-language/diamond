def surround(value, left, right)
  left + value + right
end

text = ""
x = 0
while x < 8
  text = text + "ha"
  x = x + 1
end

surround(text, "[", "]")
