# `closure name() ... end` -- a nested closure that also captures `self`,
# unlike a plain nested `def` (which never does; see docs/syntax.md and
# docs/design.md's own "closure" sections). Called immediately, in place
# -- not a define_method/redefine_method patch factory.

class Widget
  def initialize(x)
    @x = x
  end

  def helper(n) = n * 2

  def self.class_helper() = 99

  # self.foo(...), @ivar reads, and an ordinary captured local, all
  # together, called immediately.
  def run(value)
    closure inner()
      self.helper(value) + @x
    end
    inner()
  end

  # @ivar writes work too.
  def bump()
    closure inner()
      @x = @x + 1
    end
    inner()
    @x
  end

  # self.foo(...) against a *class*-owned singleton method -- self here
  # is a DIAMOND_VALUE_CLASS, not an Instance, and dispatches through the
  # same DIAMOND_OP_INVOKE_SELF_METHOD an ordinary self.foo(...) call
  # inside a real singleton method already uses.
  def self.run_class()
    closure inner()
      self.class_helper()
    end
    inner()
  end
end

w = Widget.new(10)
puts(w.run(5))       # self.helper(5)=10, +@x(10) = 20
puts(w.bump())        # 11
puts(w.bump())        # 12
puts(Widget.run_class())  # 99

# A closure declared at top level (no enclosing method, no self at all)
# is a compile-time error, not a runtime one -- see
# closure_outside_method.di/.expected_error for that case; it can't be
# exercised from inside an already-running program the way a runtime
# rescue can.

# A closure's value still can't cross Thread.new -- it captures self
# (capture_count>=1), so it's rejected the same way any other capturing
# closure already is, no new VM logic needed.
class Widget2
  def initialize(x)
    @x = x
  end
  def make()
    closure inner()
      @x
    end
    inner
  end
end
callable = Widget2.new(7).make()
begin
  t = Thread.new(callable)
  t.join()
  puts("no raise")
rescue error: TypeError
  puts("Thread.new rejected a self-capturing closure")
end

# Known, separate, pre-existing limitation -- NOT fixed by this feature:
# a nested def/closure redeclared a second time inside the same loop
# body raises a runtime TypeError at the *second* declaration. closure
# shares the underlying DIAMOND_OP_CLOSURE-emission machinery with plain
# nested def, so it inherits this too -- confirmed directly, documented
# here rather than assumed either way. See docs/roadmap.md's "Open
# design decisions" section.
class LoopWidget
  def initialize(x)
    @x = x
  end
  def run()
    i = 0
    results = []
    begin
      while i < 3
        closure inner()
          @x + i
        end
        results.push(inner())
        i += 1
      end
    rescue error: TypeError
      results.push("redeclaration TypeError at iteration #{i}")
    end
    results
  end
end
puts(LoopWidget.new(100).run())

puts("closure self-capture smoke ok")
