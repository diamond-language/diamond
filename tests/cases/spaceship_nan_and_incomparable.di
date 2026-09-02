# NaN is unorderable (matches Ruby's Float::NAN <=> 1 => nil), and so is
# an Instance with no `<=>` method of its own -- both return Nil, not a
# raised TypeError. String *is* ordered (byte-lexicographic -- see
# string_comparison.di), so it's no longer a third example of this same
# "incomparable" case; kept in this test as a positive contrast instead
# (a real, defined answer, not nil).
class Plain
  def initialize(x)
    @x = x
  end
end
"#{(0.0 / 0.0) <=> 1}, #{1 <=> (0.0 / 0.0)}, #{Plain.new(1) <=> Plain.new(2)}, #{"a" <=> "b"}"
