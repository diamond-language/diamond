interface Named
 def name()
end
class Person
 def name() -> String
  "diamond"
 end
end
def name_or_nil(value: Named | Nil)
 if value is Named
  value.name()
 else
  nil
 end
end
[name_or_nil(Person.new()), name_or_nil(nil)]
