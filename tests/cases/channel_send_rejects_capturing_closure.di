def make_capturing(x)
  def inner() = x
  inner
end
ch = Channel.new(2)
ch.send(make_capturing(5))
