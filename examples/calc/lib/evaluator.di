# Evaluation, plus a small algebraic simplifier written with object
# patterns. Integers stay integers until an operation needs a Float, and
# Diamond's Int grows past 64 bits on its own, so 2^100 is exact.
require "./ast"
require "./lexer"

def calc_builtins() -> Hash
  def calc_sqrt(x)
    raise ArgumentError.new("sqrt of a negative number") if x < 0
    sqrt(x)
  end
  def calc_abs(x) = abs(x)
  def calc_round(x) = if x is Float then x.round() else x end
  def calc_min(first, *rest)
    rest.reduce(first) do |low, x| min(low, x) end
  end
  def calc_max(first, *rest)
    rest.reduce(first) do |high, x| max(high, x) end
  end
  {
    "sqrt": [calc_sqrt, 1],
    "abs": [calc_abs, 1],
    "round": [calc_round, 1],
    "min": [calc_min, -1],
    "max": [calc_max, -1],
  }
end

class Evaluator
  def initialize()
    @vars = {"pi": 3.141592653589793, "e": 2.718281828459045}
    @builtins = calc_builtins()
  end

  def names() -> Array = @vars.keys()

  def eval(expr: Expr)
    case expr
    when Num then expr.value()
    when Var
      unless @vars.include_key?(expr.name())
        raise EvalError.new("unknown name '#{expr.name()}'", expr.column())
      end
      @vars[expr.name()]
    when Neg then -self.eval(expr.operand())
    when BinOp
      self.arith(expr.op(), self.eval(expr.left()), self.eval(expr.right()), expr.column())
    when Call then self.call(expr)
    when Assign
      value = self.eval(expr.value())
      @vars[expr.name()] = value
      value
    end
  end

  private

  def arith(op: String, a, b, column: Int)
    case op
    when "+" then a + b
    when "-" then a - b
    when "*" then a * b
    when "/", "%"
      if b == 0
        raise EvalError.new("division by zero", column)
      end
      if op == "%" then a % b
      elsif a is Int && b is Int && a % b == 0 then a / b
      else to_f(a) / b
      end
    when "^"
      if a is Int && b is Int && b >= 0 then self.int_pow(a, b) else pow(a, b) end
    end
  end

  # Exponentiation by squaring, written as a self-recursive tail call so it
  # runs in constant stack space.
  def int_pow(base: Int, exponent: Int, acc: Int = 1) -> Int
    return acc if exponent == 0
    next_acc = if exponent % 2 == 1 then acc * base else acc end
    self.int_pow(base * base, exponent / 2, next_acc)
  end

  def call(expr: Call)
    entry = @builtins[expr.name()]
    if entry == nil
      raise EvalError.new("unknown function '#{expr.name()}'", expr.column())
    end
    [function, arity] = entry
    args = expr.args().map() do |arg| self.eval(arg) end
    if (arity >= 0 && args.length() != arity) || args.empty?()
      wanted = if arity >= 0 then "#{arity}" else "at least 1" end
      raise EvalError.new("#{expr.name()} takes #{wanted} argument(s), got #{args.length()}", expr.column())
    end
    begin
      function(*args)
    rescue error: ArgumentError
      raise EvalError.new(error.message(), expr.column())
    end
  end
end

# Rewrites identities bottom-up: x+0, x*1, x*0, x^1, x^0, --x, and folds
# operations whose operands are both literal numbers.
def calc_simplify(expr: Expr) -> Expr
  case expr
  when BinOp
    left = calc_simplify(expr.left())
    right = calc_simplify(expr.right())
    calc_simplify_node(BinOp.new(expr.op(), left, right, expr.column()))
  when Neg
    case calc_simplify(expr.operand())
    when Neg{operand: inner} then inner
    when Num{value: v} then Num.new(-v)
    else Neg.new(calc_simplify(expr.operand()))
    end
  when Call
    Call.new(expr.name(), expr.args().map() do |arg| calc_simplify(arg) end, expr.column())
  when Assign then Assign.new(expr.name(), calc_simplify(expr.value()))
  when Num, Var then expr
  end
end

def calc_simplify_node(node: BinOp) -> Expr
  case node
  when BinOp{op: "+", left: Num{value: 0}, right: other},
       BinOp{op: "+", left: other, right: Num{value: 0}},
       BinOp{op: "-", left: other, right: Num{value: 0}},
       BinOp{op: "*", left: Num{value: 1}, right: other},
       BinOp{op: "*", left: other, right: Num{value: 1}},
       BinOp{op: "/", left: other, right: Num{value: 1}},
       BinOp{op: "^", left: other, right: Num{value: 1}}
    other
  when BinOp{op: "*", left: Num{value: 0}}, BinOp{op: "*", right: Num{value: 0}}
    Num.new(0)
  when BinOp{op: "^", right: Num{value: 0}}
    Num.new(1)
  when BinOp{op: "-", left: Var{name: a}, right: Var{name: b}} if a == b
    Num.new(0)
  when BinOp{left: Num{}, right: Num{}}
    begin
      Num.new(Evaluator.new().eval(node))
    rescue error: CalcError
      node   # leave 1/0 alone; evaluating it reports the error properly
    end
  else
    node
  end
end
