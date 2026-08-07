def make_pair()
  value = 0
  def increment()
    value = value + 1
  end
  def read()
    value
  end
  [increment, read]
end

pair = make_pair()
increment = pair[0]
read = pair[1]
increment()
increment()
read()
