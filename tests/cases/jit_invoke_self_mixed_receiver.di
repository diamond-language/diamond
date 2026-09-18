# A method containing BOTH a self.method() call (compiles via this phase's
# new compile_invoke_self path) AND a non-self other.method() call to a
# name that isn't dup/freeze/frozen? (unsupported, still bails) must still
# bail as a WHOLE function -- confirms the new recv==0 gate doesn't
# accidentally widen eligibility for the rest of the function. `name` (and
# `initialize`) compile; `greet_both` (the caller, containing the non-self
# call) correctly never does -- only 2 compiled, not 3, with 0 counted
# bailouts (a function that never becomes eligible in the first place
# isn't a "bailout" in DIAMOND_TRACE_JIT's own sense -- that counter is a
# runtime guard-failure count for code that DID compile; see jit_call_or_
# interpret's own jit_bailouts++ site).
class Greeter
  def initialize(name)
    @name = name
  end
  def name() = @name
  def greet_both(other)
    self_name = self.name()
    other_name = other.name()
    "#{self_name} and #{other_name}"
  end
end

a = Greeter.new("Ada")
b = Greeter.new("Grace")
puts(a.greet_both(b))
