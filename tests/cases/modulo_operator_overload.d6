class Clock
  def initialize(hours)
    @hours = hours
  end
  def %(other)
    Clock.new(mod(@hours, other))
  end
  def hours() = @hours
end
(Clock.new(26) % 24).hours()
