class Greeter
  def greeting()
    1
  end
end
class LoudGreeter < Greeter
  def greeting()
    super() + 100
  end
end
LoudGreeter.new().greeting()
