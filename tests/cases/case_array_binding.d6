class Animal
end

class Dog < Animal
end

def classify(value)
  case value
  when ["event", [1..3, name], Dog, _]
    ["matched", name]
  when ["event", [4...6, name], Animal, _]
    ["animal", name]
  else
    "other"
  end
end

puts(classify(["event", [2, "Ada"], Dog.new(), 99]))
puts(classify(["event", [4, "Grace"], Animal.new(), nil]))
puts(classify(["event", [7, "Linus"], Dog.new(), 99]))
puts(classify(["event", [2, "short"], Dog.new()]))
puts(classify("not an array"))

name = "unchanged"
case ["wrong", [2, "new"], Dog.new(), 99]
when ["event", [1..3, name], Dog, _]
  nil
end
puts(name)
