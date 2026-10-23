class Skip < StandardError
end

class Foo
  def self.skip(reason: String) -> Bool
    raise Skip.new(reason)
  end
end

result = begin
  Foo.skip("nope")
rescue e: Skip
  "caught: " + e.message()
end
result
