puts(7 % 3)
puts(-7 % 3)
puts(7.5 % 2)

class Clock
  def initialize(hours)
    @hours = hours
  end
  def %(other)
    Clock.new(@hours % other)
  end
  def hours() = @hours
end
puts((Clock.new(26) % 24).hours())

begin
  1 % 0
rescue error: ZeroDivisionError
  puts("zero division caught")
end
nil
