interface Valuable
  def value() -> Int
end

class Answer
  def value() -> Int
    42
  end
end

def read(item: Valuable) -> Int
  item.value()
end

read(Answer.new())
