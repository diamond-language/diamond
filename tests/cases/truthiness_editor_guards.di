class TruthinessPet
  def bark() = "woof"
end

def truthiness_pets() = {"pet": TruthinessPet.new()}

def truthiness_contract() -> TruthinessPet
  pet = truthiness_pets()["pet"]
  return TruthinessPet.new() unless pet
  pet
end

def classify_truthiness(value)
  if value
    "truthy"
  else
    value == nil ? "nil" : "false"
  end
end

puts(truthiness_contract().bark())
puts(classify_truthiness(false))
puts(classify_truthiness(nil))
puts(classify_truthiness(0))
puts(classify_truthiness(""))
classify_truthiness(TruthinessPet.new())
