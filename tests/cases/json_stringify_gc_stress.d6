# Encoding with non-String keys allocates a to_s String per key while the
# walk is in progress; under stress GC every one of those allocations
# collects. The result must be identical to the unstressed encoding.
class Tag
  def initialize(n)
    @n = n
  end

  def to_s()
    "tag-#{@n}"
  end
end

h = {}
i = 0
while i < 200
  h[Tag.new(i)] = {"v": i, "list": [i, "s#{i}", nil, true, 1.5]}
  i += 1
end
text = JSON.stringify(h)
puts(text.length())
puts(text[0, 60])
puts(JSON.stringify(JSON.parse(text)) == text)
