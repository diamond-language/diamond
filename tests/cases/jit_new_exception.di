# DIAMOND_OP_NEW on an Exception subclass -- confirms diamond_jit_new_
# instance's exception-class two-field special case (no `initialize`
# defined at all, so the class's own ancestor-walk finds DIAMOND_CLASS_
# EXCEPTION and sets instance->fields[0] directly from the constructor
# argument) is handled correctly, not just the ordinary initialize-call
# path jit_new_local.di already exercises. .message() dispatches through
# diamond_jit_invoke_instance's own exception-instance interception
# (Phase 7), reading back the very field this trampoline just set.
class MyError < Exception
end

def run_loop(n) -> String
  msg = ""
  i = 0
  while i < n
    e = MyError.new("boom")
    msg = e.message()
    i = i + 1
  end
  msg
end

puts(run_loop(50000))
