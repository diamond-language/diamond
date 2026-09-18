# self.dup()/freeze()/frozen?()/respond_to?()/public_send() -- confirms
# diamond_jit_invoke_instance's extraction from the interpreter's own
# DIAMOND_OP_INVOKE case kept the whole universal-method-interception tail
# intact, not trimmed. dup_self() also exercises the genuinely new
# capability this phase adds over Phase 4/6's own narrower dup/freeze/
# frozen? trampoline: OverriddenBox's own dup() override is correctly
# dispatched to (999, not a plain copy of 5) *and* compiles with zero
# bailouts -- the old path could only ever detect an override and safely
# retry via full interpretation for that one call, never actually compile
# through it.
class Box
  def initialize(v)
    @v = v
  end
  def value() = @v
  def dup_self() = self.dup()
  def freeze_self()
    self.freeze()
  end
  def frozen_self() = self.frozen?()
  def respond_self() = self.respond_to?(:value)
  def public_send_self() = self.public_send(:value)
end

class OverriddenBox < Box
  def dup() = OverriddenBox.new(999)
end

b = Box.new(10)
d = b.dup_self()
puts(d.value())

b.freeze_self()
puts(b.frozen_self())

puts(b.respond_self())
puts(b.public_send_self())

ob = OverriddenBox.new(5)
od = ob.dup_self()
puts(od.value())
