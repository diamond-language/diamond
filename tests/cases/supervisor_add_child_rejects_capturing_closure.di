x = 5
def make_capturer(x)
  def inner()
    x
  end
  inner
end
sup = Supervisor.new()
sup.add_child(make_capturer(x))
