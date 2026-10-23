# A module's @@variables are shared by its own singleton functions and the
# instance methods it mixes into classes; each module has its own.
module Registry
  def self.register(name)
    @@names ||= []
    @@names.push(name)
  end
  def self.all() = @@names
  def registered?() = @@names.include?(self.label())
end
module Other
  def self.set(value)
    @@names = value
  end
  def self.get() = @@names
end
class Widget
  include Registry
  def initialize(label)
    @label = label
  end
  def label() = @label
end
Registry.register("a")
Registry.register("b")
Other.set("separate")
[Registry.all(), Other.get(), Widget.new("a").registered?(), Widget.new("z").registered?()]
