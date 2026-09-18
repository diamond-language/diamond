# self.method[Type]() compiles to DIAMOND_OP_INVOKE_TYPED, which this
# phase deliberately never compiles (compile_body has no case for it --
# see compile_invoke_self's own comment on why: the trampoline still
# supports it for the interpreter's sake, but wiring the extra type_
# argument_count/type_arguments operands through JIT codegen is out of
# scope here, matching diamond_jit_super_call's own long-standing
# equivalent restriction for SUPER). wrap_self_typed() has no type
# variables of its own, so if INVOKE_TYPED were mistakenly compiled it
# WOULD become eligible -- staying uncompiled is the actual assertion.
class Box
  def wrap[T](value: T) -> Array[T] = [value]
  def wrap_self_typed()
    self.wrap[Int](7)
  end
end

b = Box.new()
puts(b.wrap_self_typed()[0])
