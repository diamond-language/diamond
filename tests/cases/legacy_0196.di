module Named
 def set_name(value)
  @name = value
 end
 def name() = @name
end
class Person
 include Named
end
class Product
 include Named
end
person = Person.new()
product = Product.new()
person.set_name("Ada")
product.set_name("Diamond")
[person.name(), product.name()]
