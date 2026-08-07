class Payload
  def initialize(value)
    @value = value
  end

  def value()
    @value
  end
end

def build() -> Hash
  data = {"payload": nil, "numbers": [20, 22],}
  data["payload"] = Payload.new("alive")
  data
end

result = build()
garbage = "force" + " collection"
result["numbers"][0] + result["numbers"][1]
