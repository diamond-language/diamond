class Parent
  def value() = 40
end

class Child < Parent
end

def read(object) = object.value()

parent = Parent.new()
child = Child.new()
read(parent) + read(child) + read(child) + read(parent)
