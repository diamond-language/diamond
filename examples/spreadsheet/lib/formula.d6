# The formula syntax tree. Node is sealed, so evaluate() and show() below
# stop compiling the moment a new kind is added and not handled there.

sealed class Node
end

# One class per kind of expression. Immutable; readers only.

# A literal number (always a Float; the sheet works in floating point).
class NumberNode < Node
  def initialize(value: Float)
    @value = value
  end
  def value() = @value
end

# A reference to another cell, e.g. "B2". Resolving it is the evaluator's
# job (it may be a formula itself).
class RefNode < Node
  def initialize(name: String)
    @name = name
  end
  def name() = @name
end

# Unary minus.
class NegNode < Node
  def initialize(operand: Node)
    @operand = operand
  end
  def operand() = @operand
end

# + - * /. There is no exponent or modulo in this little language.
class BinOpNode < Node
  def initialize(op: String, left: Node, right: Node)
    @op = op
    @left = left
    @right = right
  end
  def op() = @op
  def left() = @left
  def right() = @right
end

# A range function such as SUM(A1:A3): the function name and the two
# corner cells (the only argument shape the language allows).
class CallNode < Node
  def initialize(name: String, from: String, to: String)
    @name = name
    @from = from
    @to = to
  end
  def name() = @name
  def from() = @from
  def to() = @to
end

# Fully parenthesized text for a tree, e.g. for tests and debugging.
def formula_show(node: Node) -> String
  case node
  when NumberNode then "#{node.value()}"
  when RefNode then node.name()
  when NegNode then "-#{formula_show(node.operand())}"
  when BinOpNode then "(#{formula_show(node.left())} #{node.op()} #{formula_show(node.right())})"
  when CallNode then "#{node.name()}(#{node.from()}:#{node.to()})"
  end
end
