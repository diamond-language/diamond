class Base
  def initialize(data)
    current = if data["calls"] == nil then 0 else data["calls"] end
    data["calls"] = current + 1
    raise RuntimeError.new("boom")
  end
end

class Derived < Base
  attr_accessor tag: String
  def initialize(data: Hash)
    super(data)
    @tag = data["tag"]
  end
end

def run()
  row = {"tag": "x"}
  begin
    Derived.new(row)
  rescue error: RuntimeError
    nil
  end
  row["calls"]
end
run()
