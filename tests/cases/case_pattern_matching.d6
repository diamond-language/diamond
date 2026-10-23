class Animal
end

class Dog < Animal
end

class Cat < Animal
end

class EvenPattern
  def ==(value)
    value is Int && value % 2 == 0
  end
end

def classify(value)
  case value
  when 1..3
    "small"
  when 4...7
    "middle"
  when Regexp.new("^dia")
    "diamond"
  when Dog
    "dog"
  when Animal
    "animal"
  when EvenPattern.new()
    "even"
  when 9
    "nine"
  else
    "other"
  end
end

puts(classify(1))
puts(classify(3))
puts(classify(4))
puts(classify(6))
puts(classify(7))
puts(classify(2.0))
puts(classify("diamond"))
puts(classify("ruby"))
puts(classify(Dog.new()))
puts(classify(Cat.new()))
puts(classify(8))
puts(classify(9))

big = 9223372036854775807 + 1
case big
when big..big
  puts("big range")
else
  puts("missed big range")
end
