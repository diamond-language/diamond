module Values
 def value() = 42
end
class Parent
 include Values
end
class Child < Parent
end
Child.new().value()
