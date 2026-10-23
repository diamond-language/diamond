x = 5
def make_capturer(x)
  def inner() = x
  inner
end
sup = Supervisor.new()
sup.add_child(make_capturer(x))
