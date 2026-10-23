# An error raised in a compiled function with no frame of its own (no call, no
# allocation) still names it in the trace: the exit stub records the offset the
# failing arithmetic slow path stored.
class Calculator
  def divide(value)
    42 / value
  end
end

def invoke(calculator)
  calculator.divide(0)
end

invoke(Calculator.new())
