class Payload
  def initialize(value)
    @value = value
  end

  def value()
    @value
  end
end

def build()
  values = [nil]
  values[0] = Payload.new("survived")
  values
end

result = build()
garbage = "a" + "b" + "c" + "d"
result[0].value()
