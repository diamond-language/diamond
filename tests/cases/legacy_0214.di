class Parent
 private
 def answer() = 42
end
class Child < Parent
 def reveal() = self.answer()
end
Child.new().reveal()
