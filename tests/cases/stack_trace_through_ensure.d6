# The true origin frame used to go missing from an uncaught exception's
# backtrace once it passed through a begin/ensure block on its way up
# (the implicit ensure catch-all re-raised with a fresh backtrace
# instead of appending to the one already recorded at the origin).
class Calculator
  def divide(value)
    42 / value
  end
end

def invoke(calculator)
  begin
    calculator.divide(0)
  ensure
    nil
  end
end

invoke(Calculator.new())
