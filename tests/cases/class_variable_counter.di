class Counter
  def self.reset()
    @@count = 0
  end
  def initialize()
    @@count = @@count + 1
  end
  def self.count()
    @@count
  end
end
Counter.reset()
Counter.new()
Counter.new()
Counter.new()
Counter.count()
