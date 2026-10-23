# A multi-line if/elsif/else as the right-hand side of a compound
# assignment. The scan for a trailing `if` modifier only exempted the `if`
# right after a plain `=`, so `total += if ...` on its own line was read as
# a statement with a postfix condition and failed with "expected postfix
# condition" at the `end`.
total = 10
total += if total > 5
  1
elsif total > 2
  2
else
  3
end
puts(total)

text = "a"
text += if total == 11
  "b"
else
  "c"
end
puts(text)

price = 8
price -= if price > 5
  5
else
  0
end
puts(price)

scale = 3
scale *= if scale > 1
  10
else
  1
end
puts(scale)

cache = nil
cache ||= if total > 0
  "computed"
else
  "none"
end
puts(cache)

ready = true
ready &&= if total > 100
  true
else
  false
end
puts(ready)

# Still a postfix modifier when the `if` comes after the value.
count = 0
count += 5 if total > 0
count += 7 if total < 0
puts(count)

# And an indexed target.
items = [1, 2]
items[0] += if total > 0
  100
else
  0
end
puts(items[0])
