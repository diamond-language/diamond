# DIAMOND_OP_NEW on a class with no explicit `initialize` -- confirms
# diamond_jit_new_instance's own no-`initialize`, argc==0 interpreter path
# (the plain `else if(argc!=0) return DIAMOND_VM_ARITY_ERROR;` branch,
# never a call into invoke_resolved_method_helper at all) still compiles
# and produces a real, usable Instance, not just the initialize-call path
# Phase 10's own main test (jit_new_local.di) already exercises.
class Empty
  def greet() -> String = "hi"
end

def run_loop(n) -> String
  result = ""
  i = 0
  while i < n
    e = Empty.new()
    result = e.greet()
    i = i + 1
  end
  result
end

puts(run_loop(50000))
