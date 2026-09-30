class Factory
  def self.first() = Factory.later()
  def self.later() = 1
  def self.later() = 2
end
