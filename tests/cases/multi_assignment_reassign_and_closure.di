a = 100
a, b = [1, 2]

def make_pair_setter()
  x = 0
  y = 0
  def set(pair)
    x, y = pair
  end
  def get()
    [x, y]
  end
  set([3, 4])
  get()
end

[a, b, make_pair_setter()]
