# Keyword construction compiles to DIAMOND_OP_NEW_KEYWORDS, a separate,
# more complex opcode (synthetic-chunk re-entry into run_chunk) this
# phase deliberately doesn't cover -- compile_body has no case for it, so
# the containing function still bails whole, same as any other
# unsupported construct (confirmed unchanged from before this phase: a
# plain-positional-args version of this same shape compiles initialize/
# describe fine, 2 compiled -- the synthetic-chunk call path NEW_KEYWORDS
# itself uses just doesn't route through the same tier-up counting here,
# unrelated to this phase's own changes).
class Box
  def initialize(left, right)
    @left = left
    @right = right
  end
  def describe() -> String = "#{@left}-#{@right}"
end

def run_once() -> String
  box = Box.new(right: "r", left: "l")
  box.describe()
end

puts(run_once())
