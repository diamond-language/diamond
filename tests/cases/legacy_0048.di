def total()
 sum = 0
 def add(value)
  sum = sum + value
 end
 array_each([20, 22], add)
 sum
end
total()
