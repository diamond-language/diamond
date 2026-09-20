# JIT Phase 15: confirms the new invoke_site_known_class path respects
# the same whole-program redefine_method_used_anywhere gate register_
# known_class already does (Phase 12), even though nothing here is near
# the redefinition -- mirrors jit_invoke_result_redefine's own shape.
# `run` correctly never becomes eligible once ANY redefine_method call
# exists anywhere in the program.
class Widget
  def double() -> Int
    21
  end
end

class Derived
  def helper() -> Widget
    Widget.new()
  end
end

class OtherBase
  def helper() -> Widget
    Widget.new()
  end
end

class Unrelated
  def greet() = "hello"
  def self.replacement_patch()
    def greet_replacement() = "goodbye"
    greet_replacement
  end
end

Unrelated.redefine_method("greet", Unrelated.replacement_patch())

def run(x: Derived | OtherBase) -> Int
  if x is Derived
    y = x.helper()
    y.double()
  else
    0
  end
end

total = 0
i = 0
while i < 5
  total = total + run(Derived.new())
  i = i + 1
end
puts(total)
