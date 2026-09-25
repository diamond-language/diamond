# The syntax tree. Expr is sealed, so every `case` over an Expr must name
# all six node kinds (or have an `else`) -- the evaluator and printer below
# stop compiling the moment a seventh kind is added and not handled.
sealed class Expr
end

class Num < Expr
  def initialize(value: Int | Float)
    @value = value
  end
  def value() = @value
end

class Var < Expr
  def initialize(name: String, column: Int)
    @name = name
    @column = column
  end
  def name() = @name
  def column() = @column
end

class Neg < Expr
  def initialize(operand: Expr)
    @operand = operand
  end
  def operand() = @operand
end

class BinOp < Expr
  def initialize(op: String, left: Expr, right: Expr, column: Int)
    @op = op
    @left = left
    @right = right
    @column = column
  end
  def op() = @op
  def left() = @left
  def right() = @right
  def column() = @column
end

class Call < Expr
  def initialize(name: String, args: Array, column: Int)
    @name = name
    @args = args
    @column = column
  end
  def name() = @name
  def args() = @args
  def column() = @column
end

class Assign < Expr
  def initialize(name: String, value: Expr)
    @name = name
    @value = value
  end
  def name() = @name
  def value() = @value
end

# Fully parenthesized rendering, used by the `:tree` command to show how
# precedence and associativity grouped the input.
def calc_show(expr: Expr) -> String
  case expr
  when Num then "#{expr.value()}"
  when Var then expr.name()
  when Neg then "(-#{calc_show(expr.operand())})"
  when BinOp then "(#{calc_show(expr.left())} #{expr.op()} #{calc_show(expr.right())})"
  when Call then "#{expr.name()}(#{expr.args().map() do |arg| calc_show(arg) end.join(", ")})"
  when Assign then "#{expr.name()} = #{calc_show(expr.value())}"
  end
end
