class InferredHashPet
end

def inferred_guard_pets() = {"name": InferredHashPet.new()}

def checked_guard_budget(value: String | Nil) -> String
  pet0 = inferred_guard_pets()["name"]
  pet1 = inferred_guard_pets()["name"]
  pet2 = inferred_guard_pets()["name"]
  pet3 = inferred_guard_pets()["name"]
  pet4 = inferred_guard_pets()["name"]
  pet5 = inferred_guard_pets()["name"]
  pet6 = inferred_guard_pets()["name"]
  pet7 = inferred_guard_pets()["name"]
  if pet0 != nil && pet1 != nil && pet2 != nil && pet3 != nil &&
     pet4 != nil && pet5 != nil && pet6 != nil && pet7 != nil && value != nil
    value
  else
    "fallback"
  end
end

puts(checked_guard_budget("ok"))
checked_guard_budget(nil)
