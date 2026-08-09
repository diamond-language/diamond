def outer(x)
  def shadowed(x)
    x * 100
  end
  shadowed(2) + x
end
outer(9)
