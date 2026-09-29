# The formula syntax tree. Node is sealed, so evaluate() and show() below
# stop compiling the moment a new kind is added and not handled there.

sealed class Node
end

class NumberNode < Node
  def initialize(value: Float)
    @value = value
  end
  def value() = @value
end

class RefNode < Node
  def initialize(name: String)
    @name = name
  end
  def name() = @name
end

class NegNode < Node
  def initialize(operand: Node)
    @operand = operand
  end
  def operand() = @operand
end

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

def formula_show(node: Node) -> String
  case node
  when NumberNode then "#{node.value()}"
  when RefNode then node.name()
  when NegNode then "-#{formula_show(node.operand())}"
  when BinOpNode then "(#{formula_show(node.left())} #{node.op()} #{formula_show(node.right())})"
  when CallNode then "#{node.name()}(#{node.from()}:#{node.to()})"
  end
end
