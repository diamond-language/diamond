# A Pratt (precedence-climbing) parser. Each infix operator has a binding
# power; `parse_expr(min)` keeps consuming operators that bind tighter than
# `min`. Right-associative operators (`^`, `=`) recurse with one less.
require "./ast"
require "./lexer"

class Parser
  def initialize(tokens: Array[Token])
    @tokens = tokens
    @pos = 0
  end

  def self.parse(line: String) -> Expr
    parser = Parser.new(calc_tokenize(line))
    expr = parser.parse_expr(0)
    parser.expect_end()
    expr
  end

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

  def expect_end()
    token = self.peek()
    unless token.kind() == :end
      raise ParseError.new("unexpected '#{token.text()}'", token.column())
    end
  end

  private

  def peek() -> Token = @tokens[@pos]

  def advance() -> Token
    token = @tokens[@pos]
    @pos += 1
    token
  end

  def expect_op(text: String)
    token = self.advance()
    unless token.kind() == :op && token.text() == text
      found = if token.kind() == :end then "end of input" else "'#{token.text()}'" end
      raise ParseError.new("expected '#{text}' but found #{found}", token.column())
    end
  end

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

  def parse_prefix() -> Expr
    token = self.advance()
    case [token.kind(), token.text()]
    when [:number, text]
      value = if text.index_of(".") == nil then text.to_i() else text.to_f() end
      Num.new(value)
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
    when [:op, "("]
      inner = self.parse_expr(0)
      self.expect_op(")")
      inner
    when [:end, _]
      raise ParseError.new("unexpected end of input", token.column())
    else
      raise ParseError.new("unexpected '#{token.text()}'", token.column())
    end
  end

  def parse_infix(left: Expr, token: Token, power: Int) -> Expr
    case token.text()
    when "="
      unless left is Var
        raise ParseError.new("can only assign to a name", token.column())
      end
      Assign.new(left.name(), self.parse_expr(power - 1))
    when "^"
      BinOp.new("^", left, self.parse_expr(power - 1), token.column())
    else
      BinOp.new(token.text(), left, self.parse_expr(power), token.column())
    end
  end

  def parse_args() -> Array
    args = []
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
