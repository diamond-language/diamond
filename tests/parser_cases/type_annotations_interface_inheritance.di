interface Valuable
  def value() -> Int
end

interface Labeled < Valuable
  def label() -> String
end

class Answer
  def value() -> Int
    42
  end

  def label() -> String
    "answer"
  end
end

def read(item: Labeled) -> Int
  item.value()
end

read(Answer.new())
