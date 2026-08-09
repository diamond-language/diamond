class Person
 attr_reader name
 attr_writer name
end
person = Person.new()
person.name=("Ada")
person.name()
