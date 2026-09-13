class Tagged
  attr_accessor first: String, second: String, third: String

  def initialize(data: Hash)
    @first = data["a"]
    @second = data["b"]
    @third = data["c"]
  end
end

def run()
  total = 0
  index = 0
  while index < 200
    row = {"a": "alpha", "b": "beta", "c": "gamma"}
    tagged = Tagged.new(row)
    total = total + tagged.first().length() + tagged.second().length() + tagged.third().length()
    index = index + 1
  end
  total
end
run()
