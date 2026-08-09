class Counter
  def value()
    42
  end
end

def read(object)
  object.value()
end

counter = Counter.new()
index = 0
result = 0
while index < 100
  result = read(counter)
  index = index + 1
end
result
