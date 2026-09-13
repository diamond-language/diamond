class Base
  def initialize(data)
    current = if data["calls"] == nil then 0 else data["calls"] end
    data["calls"] = current + 1
  end
end

class Derived < Base
  attr_accessor tag
  def initialize(data: Hash, bad_receiver)
    super(data)
    @tag = bad_receiver["x"]
  end
end

def run()
  row = {}
  begin
    Derived.new(row, 42)
  rescue error: TypeError
    nil
  end
  row["calls"]
end
run()
