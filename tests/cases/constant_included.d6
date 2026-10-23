# Constants of an included module are visible to the including class (and to
# an including module), also from a method defined above the `include`.
module Limits
  MAX = 10
end
module Defaults
  include Limits
  NAME = "defaults"
  def self.max() = MAX
end
class Widget
  def early() = MAX
  include Defaults
  def late() = NAME + ":" + MAX.to_s()
end
puts(Widget.new().early())
puts(Widget.new().late())
puts(Widget::MAX)
puts(Widget::NAME)
puts(Defaults.max())
puts(Defaults::MAX)
