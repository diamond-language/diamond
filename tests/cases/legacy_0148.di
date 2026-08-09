class Parent
 def to_s() -> String = "parent"
end
class Child < Parent
end
"#{Child.new()}"
