class Animal
end
class Dog < Animal
end
def singleton[T](value: T) -> Array[T] = [value]
result = singleton(Dog.new())
begin
 result.push(Animal.new())
rescue error: TypeError
 42
end
