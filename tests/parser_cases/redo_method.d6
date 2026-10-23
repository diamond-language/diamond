class Counter
  def run() -> Int
    visits = 0
    loop do
      visits = visits + 1
      if visits == 1
        redo
      end
      break visits
    end
  end
end

puts(Counter.new().run())
