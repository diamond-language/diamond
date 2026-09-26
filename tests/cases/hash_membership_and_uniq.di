# include_key? is a hash lookup; fetch falls back only for an absent key;
# uniq keeps ==-semantics for instances with their own ==.
class Tag
  attr_reader name: String
  def initialize(name: String)
    @name = name
  end
  def ==(other) = other is Tag && other.name() == @name
end
h = {"a": 1, "blank": nil, 2: "two"}
membership = [h.include_key?("a"), h.include_key?("blank"), h.include_key?("zz"), h.include_key?(2.0)]
fetched = [h.fetch("blank", "fallback"), h.fetch("zz", "fallback")]
uniques = [3, 1, 3, 1.0, "a", "a", nil, nil, :s, :s].uniq()
tags = [Tag.new("x"), Tag.new("x"), Tag.new("y")].uniq().length()
[membership, fetched, uniques, tags, [1, [2, [3, [4]]], [], 5].flatten()]
