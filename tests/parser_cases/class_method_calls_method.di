class Calculator
  def initialize(base)
    @base = base
  end
  def doubled()
    self.base_value() * 2
  end
  def base_value()
    @base
  end
end
Calculator.new(21).doubled()
