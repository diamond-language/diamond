class JoinLeft
  def value() -> Int = 42
  def items() -> Array[Int] = [42]
  def values() -> Hash[String, Int] = {"answer": 42}
  def choose(value: Int) -> Int = value
  def transform(value: Int, &block: Callable[[Int], Int]) -> Int = yield(value)
end

class JoinRight
  def value() -> String = "forty-two"
  def items() -> Array[String] = ["forty-two"]
  def values() -> Hash[String, String] = {"answer": "forty-two"}
  def choose(value: Int) -> String = "forty-two"
  def transform(value: Int, &block: Callable[[Int], Int]) -> String = "forty-two"
end

def accept_value(value: Int | String) = value
def accept_items(items: Array[Int] | Array[String]) = items
def accept_values(values: Hash[String, Int] | Hash[String, String]) = values
def accept_nullable(value: Int | String | Nil) = value

receiver = if ARGV.length() == 0
  JoinLeft.new()
else
  JoinRight.new()
end

puts(accept_value(receiver.value()))
puts(accept_value(accept_items(receiver.items())[0]))
puts(accept_nullable(accept_values(receiver.values())["answer"]))
puts(accept_value(receiver.choose(42)))
puts(accept_value(receiver.choose(value: 42)))
puts(accept_value(receiver.choose(*[42])))
puts(accept_value(receiver.transform(40) do |value|
  value + 2
end))
