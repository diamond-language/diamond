class TruthyHashPet
end

def inferred_guard_pets() = {"name": TruthyHashPet.new()}

def checked_guard_budget(value: String | Nil) -> String
  pet0 = inferred_guard_pets()["name"]
  pet1 = inferred_guard_pets()["name"]
  pet2 = inferred_guard_pets()["name"]
  pet3 = inferred_guard_pets()["name"]
  pet4 = inferred_guard_pets()["name"]
  pet5 = inferred_guard_pets()["name"]
  pet6 = inferred_guard_pets()["name"]
  pet7 = inferred_guard_pets()["name"]
  if pet0 && pet1 && pet2 && pet3 &&
     pet4 && pet5 && pet6 && pet7 && value != nil
    value
  else
    "fallback"
  end
end

puts(checked_guard_budget("ok"))
checked_guard_budget(nil)
