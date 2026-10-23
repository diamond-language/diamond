class ShortCircuitPet
  def bark() = "woof"
end

def short_circuit_pets() = {"pet": ShortCircuitPet.new()}

def must_skip()
  raise("evaluated skipped operand")
end

pet = short_circuit_pets()["pet"]
missing = short_circuit_pets()["missing"]
puts(pet && pet.bark())
puts(missing == nil || missing.bark())
puts(false && must_skip())
puts(true || must_skip())
puts(nil || "fallback")
puts(0 && "zero")
"" && "empty"
