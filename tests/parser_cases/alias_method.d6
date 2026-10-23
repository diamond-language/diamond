class Answer
 def value() = 42
 alias_method result, value
end
puts(Answer.new().result())

module Named
 attr_reader name
 alias_method label, name
end
class Person
 include Named
end
puts(Person.new().label())
nil
