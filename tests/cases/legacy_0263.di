module Named
 attr_accessor name
 alias_method label=, name=
end
class Person
 include Named
end
person=Person.new()
person.label=("Ada")
person.name()
