module Outer
  def self.first() = Outer.later()
  class Inner
    def self.first() = Outer::Inner.later()
    def self.later() = 20
  end
  def self.later() = 10
end
class Parent
  def self.later() = 1
end
class Child < Parent
  def self.first() = Child.later()
  def self.later() = 2
end
puts(Outer.first())
puts(Outer::Inner.first())
puts(Child.first())
puts(Parent.later())
