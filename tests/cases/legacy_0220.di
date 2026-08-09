module Named
 attr_reader name
 attr_writer name
end
class Person
 include Named
end
person = Person.new()
person.name=("Ada")
person.name()
