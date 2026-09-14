class RecordBase
  attr_accessor id_seed
  def initialize(seed)
    @id_seed = seed
  end
end

class HydratedRecord < RecordBase
  attr_accessor email: String, role: String, is_seed

  def initialize(attributes: Hash = {})
    super(1)
    @email = attributes["email"]
    @role = if attributes["role"] == nil then "user" else attributes["role"] end
    @is_seed = attributes["is_seed"] == true
  end
end

def run()
  total = 0
  index = 0
  while index < 100
    row = {"email": "user#{index}@example.com", "role": "user", "is_seed": false}
    record = HydratedRecord.new(row)
    total = total + record.email().length() + record.id_seed()
    index = index + 1
  end
  index = 0
  while index < 100
    record = HydratedRecord.new()
    total = total + record.role().length() + record.id_seed()
    index = index + 1
  end
  total
end
run()
