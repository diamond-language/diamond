# Regex literals, and a slash that is still division after a value.
digits = "a1 b22 c333".scan(/[0-9]+/)
swapped = "2024-01-15".gsub(/(\d+)-(\d+)-(\d+)/, "\\3/\\2/\\1")
folded = /hello/i.match?("Say HELLO")
slash = "a/b".split(/\//)
cased = case "price: 42"
  when /^cost/ then "cost"
  when /\d+/ then "number"
  else "other"
end
x = 10
half = x / 2
quarter = (x + 2) / 3 / 2
[digits, swapped, folded, slash, cased, half, quarter]
