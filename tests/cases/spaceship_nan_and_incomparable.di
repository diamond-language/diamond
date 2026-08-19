# NaN is unorderable (matches Ruby's Float::NAN <=> 1 => nil), and so is
# any pair with no defined comparison (an Instance with no `<=>` method,
# or a native type -- like String -- that `<=>` deliberately isn't
# extended to). All return Nil, not a raised TypeError.
class Plain
  def initialize(x)
    @x = x
  end
end
"#{(0.0 / 0.0) <=> 1}, #{1 <=> (0.0 / 0.0)}, #{Plain.new(1) <=> Plain.new(2)}, #{"a" <=> "b"}"
