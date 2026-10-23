class State
 def valid?() = true
 def reset!() = 42
 alias_method acceptable?, valid?
 alias_method clear!, reset!
end
state=State.new()
[state.acceptable?(), state.clear!()]
