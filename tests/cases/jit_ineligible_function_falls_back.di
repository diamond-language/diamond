def double_it(value) = value * 2

def run()
  total = 0
  index = 0
  while index < 50
    total = total + double_it(index)
    index = index + 1
  end
  total
end
run()
