class Dog
end

class Cat
end

def animal(use_dog: Bool) -> Dog | Cat
  if use_dog
    Dog.new()
  else
    Cat.new()
  end
end

def require_dog(use_dog: Bool) -> Dog
  value = animal(use_dog)
  if value is Dog
    return value
  else
    return Dog.new()
  end
end

require_dog(false) is Dog
