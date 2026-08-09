x = 0
sum = 0

while x < 10
  x = x + 1
  if x > 5
    sum = sum + x
  else
    sum = sum
  end
end

if sum == 40
  sum + 2
else
  0
end
