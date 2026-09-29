# A precedence-climbing parser for one cell's formula, the same shape as
# examples/calc's own -- but the grammar is narrower (no assignment, no
# variables, and a call's only argument shape is a REF:REF range).
require "./formula"
require "./lexer"

class Parser
  def initialize(tokens: Array[Token])
    @tokens = tokens
    @pos = 0
  end

  def self.parse(text: String) -> Node
    parser = Parser.new(formula_tokenize(text))
    node = parser.parse_expr(0)
    parser.expect_end()
    node
  end

  def parse_expr(min_power: Int) -> Node
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
      raise FormulaError.new("unexpected '#{token.text()}'", token.column())
    end
  end

  private

  def peek() -> Token = @tokens[@pos]

  def advance() -> Token
    token = @tokens[@pos]
    @pos += 1
    token
  end

  def expect_op(text: String) -> Token
    token = self.advance()
    unless token.kind() == :op && token.text() == text
      found = if token.kind() == :end then "end of input" else "'#{token.text()}'" end
      raise FormulaError.new("expected '#{text}' but found #{found}", token.column())
    end
    token
  end

  def expect_ref() -> Token
    token = self.advance()
    unless token.kind() == :ref
      found = if token.kind() == :end then "end of input" else "'#{token.text()}'" end
      raise FormulaError.new("expected a cell reference but found #{found}", token.column())
    end
    token
  end

  def infix_power(token: Token) -> Int
    return 0 unless token.kind() == :op
    case token.text()
    when "+", "-" then 10
    when "*", "/" then 20
    else 0
    end
  end

  def parse_prefix() -> Node
    token = self.advance()
    case [token.kind(), token.text()]
    when [:number, text]
      NumberNode.new(text.to_f())
    when [:ref, name]
      RefNode.new(name)
    when [:name, name]
      self.expect_op("(")
      from = self.expect_ref().text()
      self.expect_op(":")
      to = self.expect_ref().text()
      self.expect_op(")")
      CallNode.new(name, from, to)
    when [:op, "-"]
      NegNode.new(self.parse_expr(30))
    when [:op, "("]
      inner = self.parse_expr(0)
      self.expect_op(")")
      inner
    when [:end, _]
      raise FormulaError.new("unexpected end of input", token.column())
    else
      raise FormulaError.new("unexpected '#{token.text()}'", token.column())
    end
  end

  def parse_infix(left: Node, token: Token, power: Int) -> Node
    BinOpNode.new(token.text(), left, self.parse_expr(power))
  end
end
