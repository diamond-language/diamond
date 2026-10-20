class Alpha
  def value() = 1
end

class Beta
  def value() = 2
end

def read(object) = object.value()

alpha = Alpha.new()
beta = Beta.new()
index = 0
total = 0
while index < 500
  total = total + read(alpha)
  index = index + 1
end
index = 0
while index < 500
  total = total + read(beta)
  index = index + 1
end
total
