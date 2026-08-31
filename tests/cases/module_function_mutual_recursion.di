# Mutual recursion between two *different* module_function siblings,
# where the caller is compiled *before* the callee in the same module --
# `is_even` (compiled first) calling `is_odd` (defined later) -- used to
# fail to compile ("undefined module singleton function"), a harder
# problem than plain self-recursion (see module_function_self_
# recursion.di): even the discovery pass hits the same forward-reference
# problem before any cross-pass carryover could help, since it too
# compiles method bodies in source order. Fixed by
# prescan_module_function_signatures (src/compiler.c): a module's own
# lookahead pre-scan registers every module_function/`def self.x`
# signature it finds -- name and accurate arity, computed the same way
# compile_definition's own parameter loop does -- before compiling any
# of their bodies, so an earlier sibling's forward call gets a pending
# call-site fixup (see DIAMOND_UNRESOLVED_SINGLETON_FUNCTION) resolved
# once the real def actually compiles, in normal source order, with no
# change to how functions are numbered/reserved elsewhere.
require "../../lib/minitest"

module Parity
  module_function

  def is_even(n)
    if n == 0
      true
    else
      Parity.is_odd(n - 1)
    end
  end

  def is_odd(n)
    if n == 0
      false
    else
      Parity.is_even(n - 1)
    end
  end
end

# `def self.x` form of the same forward reference, not just
# module_function -- a separate registration site (compile_module's own
# `def self.x` handling) needed the identical fix.
module ParitySelf
  def self.is_even(n)
    if n == 0
      true
    else
      ParitySelf.is_odd(n - 1)
    end
  end

  def self.is_odd(n)
    if n == 0
      false
    else
      ParitySelf.is_even(n - 1)
    end
  end
end

# A module with several `def self.x` methods, called only externally --
# never forward-referencing each other -- confirms the pre-scan doesn't
# break the far more common ordinary case. Specifically regresses a real
# bug found while building this fix: the pre-scan's placeholder entries
# defaulted needs_receiver to true (correct for module_function, wrong
# for `def self.x`, which reserves no implicit receiver slot at all),
# inflating the compiled call's own argument count by one and failing
# the real def's arity check at runtime ("wrong number of arguments").
module Widgets
  def self.table(name) = "table:" + name
  def self.cte(name) = "cte:" + name
end

def run_tests()
  def test_module_function_mutual_recursion_even()
    Minitest.assert_equal(true, Parity.is_even(10))
  end

  def test_module_function_mutual_recursion_odd()
    Minitest.assert_equal(true, Parity.is_odd(7))
  end

  def test_def_self_mutual_recursion()
    Minitest.assert_equal(true, ParitySelf.is_even(10))
  end

  def test_def_self_ordinary_external_call_unaffected()
    Minitest.assert_equal("table:authors", Widgets.table("authors"))
    Minitest.assert_equal("cte:recent", Widgets.cte("recent"))
  end

  suite = Minitest.new()
  suite.test("module_function mutual recursion, forward reference (is_even)",
             test_module_function_mutual_recursion_even)
  suite.test("module_function mutual recursion, forward reference (is_odd)",
             test_module_function_mutual_recursion_odd)
  suite.test("def self.x mutual recursion, forward reference",
             test_def_self_mutual_recursion)
  suite.test("def self.x ordinary external call, no forward reference",
             test_def_self_ordinary_external_call_unaffected)
  suite.run!()
end

run_tests()
