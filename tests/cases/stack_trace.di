class Calculator
  def divide(value)
    42 / value
  end
end

def invoke(calculator)
  calculator.divide(0)
end

invoke(Calculator.new())
