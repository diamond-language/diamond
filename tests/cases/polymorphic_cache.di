class Forty
  def value()
    40
  end
end

class Two
  def value()
    2
  end
end

def read(object)
  object.value()
end

forty = Forty.new()
two = Two.new()
read(forty)
read(two)
read(forty) + read(two)
