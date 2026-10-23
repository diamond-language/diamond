# Evaluation, plus a small algebraic simplifier written with object
# patterns. Integers stay integers until an operation needs a Float, and
# Diamond's Int grows past 64 bits on its own, so 2^100 is exact.
require "./ast"
require "./lexer"

# The built-in functions, as a Hash from name to [function, arity]. Arity -1
# means variadic (at least one argument). The functions are nested `def`s
# so they stay private to this table, and are stored as values in the Hash.
def calc_builtins() -> Hash
  # Raises ArgumentError (not EvalError, which needs a column this function
  # does not know); Evaluator#call catches it and re-raises with the column.
  def calc_sqrt(x)
    raise ArgumentError.new("sqrt of a negative number") if x < 0
    sqrt(x)
  end

  def calc_abs(x) = abs(x)
  # Rounding an Int is a no-op, so it stays an Int.
  def calc_round(x) = if x is Float then x.round() else x end

  # min/max: `first` plus `*rest` makes at least one argument required, and
  # the reduce seeds from `first`.
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

# Evaluates a syntax tree. State is just the variable table, so a session
# keeps its assignments between lines.
class Evaluator
  def initialize()
    # Predefined constants.
    @vars = {"pi": 3.141592653589793, "e": 2.718281828459045}
    @builtins = calc_builtins()
  end

  def names() -> Array = @vars.keys()

  # Walks the tree. The `case` is exhaustive over the sealed Expr, so the
  # compiler checks that every node kind is handled.
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

  # Applies one binary operator. Integer results stay Int where they can:
  # `+ - *` on Ints give Ints, and Diamond's Int grows past 64 bits
  # automatically, so large results stay exact.
  def arith(op: String, a, b, column: Int)
    case op
    when "+" then a + b
    when "-" then a - b
    when "*" then a * b
    when "/", "%"
      # Check for zero first (it is an error for both operators), then
      # choose the result type. `/` is exact (Int) when the Ints divide
      # evenly (6 / 3 => 2) and a Float otherwise (7 / 2 => 3.5); `%` is
      # always the remainder.
      if b == 0
        raise EvalError.new("division by zero", column)
      end

      if op == "%" then a % b
      elsif a is Int && b is Int && a % b == 0 then a / b
      else to_f(a) / b
      end
    when "^"
      # A non-negative Int exponent uses exact integer power. Anything else
      # (a Float, or a negative exponent) falls back to floating point.
      if a is Int && b is Int && b >= 0 then self.int_pow(a, b) else pow(a, b) end
    end
  end

  # Exponentiation by squaring, written as a self-recursive tail call so it
  # runs in constant stack space.
  # The accumulator `acc` carries the answer so the recursive call is the
  # LAST thing the method does, which is what makes it a tail call. Each step
  # squares the base and halves the exponent; when the exponent is odd, the
  # current base is multiplied into `acc` first.
  def int_pow(base: Int, exponent: Int, acc: Int = 1) -> Int
    return acc if exponent == 0

    next_acc = if exponent % 2 == 1 then acc * base else acc end
    self.int_pow(base * base, exponent / 2, next_acc)
  end

  # Calls a built-in. Arguments are evaluated first, then the arity is
  # checked, then the function runs.
  def call(expr: Call)
    entry = @builtins[expr.name()]
    if entry == nil
      raise EvalError.new("unknown function '#{expr.name()}'", expr.column())
    end

    # Destructure the [function, arity] pair from the table.
    [function, arity] = entry
    args = expr.args().map() do |arg| self.eval(arg) end

    # Fixed-arity functions need exactly that many arguments; variadic ones
    # (arity -1) just need at least one.
    if (arity >= 0 && args.length() != arity) || args.empty?()
      wanted = if arity >= 0 then "#{arity}" else "at least 1" end
      raise EvalError.new("#{expr.name()} takes #{wanted} argument(s), got #{args.length()}", expr.column())
    end

    # `*args` spreads the Array into separate arguments. A built-in's own
    # ArgumentError (like sqrt of a negative) is turned into a CalcError that
    # carries the call's column.
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
    # Simplify both children first (bottom-up), rebuild the node, then try
    # to simplify the node itself now that its operands are in final form.
    left = calc_simplify(expr.left())
    right = calc_simplify(expr.right())
    calc_simplify_node(BinOp.new(expr.op(), left, right, expr.column()))
  when Neg
    # Double negation cancels (--x => x), and negating a literal folds.
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

# Simplifies ONE BinOp whose children are already simplified. These are
# object patterns: `BinOp{op: "+", left: Num{value: 0}, right: other}` matches
# a "+" whose left side is the literal 0 and binds the right side to `other`.
# The first `when` lists several alternatives that all bind the same `other`
# and all mean "the operation changes nothing, return the other operand".
# Later arms are checked only if earlier ones did not match.
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
  # Anything times 0 is 0; anything to the power 0 is 1.
  when BinOp{op: "*", left: Num{value: 0}}, BinOp{op: "*", right: Num{value: 0}}
    Num.new(0)
  when BinOp{op: "^", right: Num{value: 0}}
    Num.new(1)
  # x - x is 0, but only when both sides are the SAME variable: the guard
  # (`if a == b`) compares the two bound names.
  when BinOp{op: "-", left: Var{name: a}, right: Var{name: b}} if a == b
    Num.new(0)
  # Two literals: just compute it, by running the real evaluator on this one
  # node, so folding can never disagree with evaluation.
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
