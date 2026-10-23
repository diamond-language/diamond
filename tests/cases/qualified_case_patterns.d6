module Events
  class Score
    attr_reader points: Int
    def initialize(points: Int)
      @points = points
    end
  end
end

event = Events::Score.new(7)
case event
when Events::Score{points: points}
  puts(points)
else
  puts("miss")
end

case event
when Events::Score
  puts("class")
else
  puts("miss")
end
nil
