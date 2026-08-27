class Score
  attr_accessor(value)

  def initialize(value)
    @value = value
  end
end

class Player
  attr_accessor(name, score)

  def initialize(name, score)
    @name = name
    @score = score
  end
end

first = Player.new("old", Score.new(0))
second = Player.new("older", Score.new(1))
[first.name, first.score.value] = ["alice", 10]
puts(first.name())
puts(first.score().value())

players = [first, second]
players[1].name, players[1].score.value = "bob", 20
puts(second.name())
puts(second.score().value())

begin
  [first.name, [first.score.value, ignored]] = ["corrupt", [99]]
rescue error: ArgumentError
  puts(error.message())
end
puts(first.name())
puts(first.score().value())
