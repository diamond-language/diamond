def outer()
  y = 5
  def inner()
    y = y + 1
    y
  end
  inner()
  inner()
  y
end
outer()
