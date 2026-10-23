# A Pratt (precedence-climbing) parser. Each infix operator has a binding
# power; `parse_expr(min)` keeps consuming operators that bind tighter than
# `min`. Right-associative operators (`^`, `=`) recurse with one less.
require "./ast"
require "./lexer"

# A Parser consumes one token Array; `@pos` is the next unread token.
class Parser
  def initialize(tokens: Array[Token])
    @tokens = tokens
    @pos = 0
  end

  # Entry point: tokenize, parse one expression, and insist nothing is left
  # over (so "1 2" is an error, not just "1").
  def self.parse(line: String) -> Expr
    parser = Parser.new(calc_tokenize(line))
    expr = parser.parse_expr(0)
    parser.expect_end()
    expr
  end

  # The core of the algorithm. Parse the thing that starts an expression
  # (a number, a name, "-", "("), then keep absorbing infix operators WHILE
  # each binds tighter than `min_power`. A tighter operator takes the
  # left side for itself (a + b * c: `*` grabs b); an operator no tighter
  # than the caller's own ends this call so the caller can use it (a * b +
  # c: `+` is left to the outer level). Non-operators have power 0, so
  # they always stop the loop.
  def parse_expr(min_power: Int) -> Expr
    left = self.parse_prefix()

    loop do
      token = self.peek()
      power = self.infix_power(token)
      break if power <= min_power

      self.advance()
      left = self.parse_infix(left, token, power)
    end
    left
  end

  # Raises unless every token has been consumed.
  def expect_end()
    token = self.peek()
    unless token.kind() == :end
      raise ParseError.new("unexpected '#{token.text()}'", token.column())
    end
  end

  private

  # Look at the next token without consuming it.
  def peek() -> Token = @tokens[@pos]

  # Consume and return the next token.

  def advance() -> Token
    token = @tokens[@pos]
    @pos += 1
    token
  end

  # Consume the next token, which must be this exact operator (e.g. the ")"
  # closing a group).
  def expect_op(text: String)
    token = self.advance()
    unless token.kind() == :op && token.text() == text
      found = if token.kind() == :end then "end of input" else "'#{token.text()}'" end
      raise ParseError.new("expected '#{text}' but found #{found}", token.column())
    end
  end

  # Binding power of an infix operator; higher binds tighter. 0 means "not an
  # infix operator", which ends an expression. Gaps between the numbers leave
  # room for `-` (30, below) to sit between * and ^.
  def infix_power(token: Token) -> Int
    return 0 unless token.kind() == :op

    case token.text()
    when "=" then 1
    when "+", "-" then 10
    when "*", "/", "%" then 20
    when "^" then 40
    else 0
    end
  end

  # Parses whatever can START an expression. Matches on [kind, text] pairs.
  def parse_prefix() -> Expr
    token = self.advance()

    case [token.kind(), token.text()]
    # A literal: no "." means Int, otherwise Float.
    when [:number, text]
      value = if text.index_of(".") == nil then text.to_i() else text.to_f() end
      Num.new(value)
    # A name followed by "(" is a call; otherwise a variable.
    when [:name, name]
      if self.peek().text() == "("
        self.advance()
        Call.new(name, self.parse_args(), token.column())
      else
        Var.new(name, token.column())
      end
    when [:op, "-"]
      # Binds looser than ^, so -2^2 is -(2^2), as in mathematics.
      Neg.new(self.parse_expr(30))
    # A parenthesized group restarts at power 0, so it can hold anything.
    when [:op, "("]
      inner = self.parse_expr(0)
      self.expect_op(")")
      inner
    # Input ended where an operand was required (e.g. "1 +").
    when [:end, _]
      raise ParseError.new("unexpected end of input", token.column())
    else
      raise ParseError.new("unexpected '#{token.text()}'", token.column())
    end
  end

  # Builds the node for an infix operator whose left side is already parsed.
  # The recursion's argument sets ASSOCIATIVITY: passing `power` makes the
  # operator left-associative (a - b - c is (a - b) - c, since the next `-`
  # is not tighter than `power` and stops the right side); passing
  # `power - 1` lets an equal operator continue the right side, making it
  # right-associative (a ^ b ^ c is a ^ (b ^ c); a = b = c assigns right to
  # left).
  def parse_infix(left: Expr, token: Token, power: Int) -> Expr
    case token.text()
    # Assignment: the left side must be a bare name.
    when "="
      unless left is Var
        raise ParseError.new("can only assign to a name", token.column())
      end
      Assign.new(left.name(), self.parse_expr(power - 1))
    # Right-associative.
    when "^"
      BinOp.new("^", left, self.parse_expr(power - 1), token.column())
    # Every other operator is left-associative.
    else
      BinOp.new(token.text(), left, self.parse_expr(power), token.column())
    end
  end

  # Parses call arguments after the "(" has been consumed: nothing but ")" for
  # zero arguments, else comma-separated expressions, then ")".
  def parse_args() -> Array
    args = []

    # Empty argument list.
    if self.peek().text() == ")"
      self.advance()
      return args
    end

    loop do
      args.push(self.parse_expr(0))
      break if self.peek().text() != ","
      self.advance()
    end
    self.expect_op(")")
    args
  end
end
