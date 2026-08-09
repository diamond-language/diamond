interface Named
 def name()
end
class Parent
 def name() -> String
  "diamond"
 end
end
class Child < Parent
end
Child.new() is Named
