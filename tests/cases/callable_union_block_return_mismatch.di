class UnionReturnAnimal
end

class UnionReturnDog < UnionReturnAnimal
end

def require_dog(&block: Callable[[], UnionReturnDog]) -> Int = 1
def require_animal(&block: Callable[[], UnionReturnAnimal]) -> Int = 1

consumer = if ARGV.length() == 0
  require_dog
else
  require_animal
end

consumer() do
  "wrong"
end
