def make_adder(amount)
  def add(value)
    amount + value
  end
  add
end

add_forty = make_adder(40)
add_two = make_adder(2)
add_forty(2) + add_two(3)
