def accept_int(value: Int) = value
def accept_numeric(value: Int | Float) = value

def sum_int(values: Array[Int])
  accept_int(values.sum())
end

def sum_float(values: Array[Float])
  accept_numeric(values.sum())
end

def sum_mixed(values: Array[Int | Float])
  accept_numeric(values.sum())
end

puts(sum_int([1, 2]) == 3)
puts(sum_float([1.5, 2.5]) == 4.0)
puts(sum_mixed([1, 2.5]) == 3.5)
