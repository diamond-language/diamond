# Evaluates a whole sheet: memoized per cell, and safe against a formula
# that (directly or through several other cells) refers back to itself.
require "./cellref"
require "./formula"
require "./parser"

class CircularReferenceError < StandardError
end

class Sheet
  def initialize()
    # @raw: name -> the text exactly as written in the file.
    # @cache: name -> its finished value (memoization).
    # @visiting: the cells currently being evaluated, innermost last; it is
    # the call stack of value_of, kept explicitly so a cycle can be detected
    # and reported as a chain.
    @raw = {}
    @cache = {}
    @visiting = []
  end

  # Store a cell's raw text. Nothing is parsed or evaluated until asked.
  def set(name: String, raw_text: String)
    @raw[name] = raw_text
  end

  def names() -> Array = @raw.keys()

  # A cell's evaluated value: the Float a formula/number produces, or the
  # String a label holds. Each name is evaluated at most once per Sheet --
  # @cache holds every name this run has ever finished evaluating, even a
  # plain label with no dependents of its own.
  def value_of(name: String) -> Float | String
    # Already finished: reuse it.
    return @cache[name] if @cache.include_key?(name)

    # If this cell is already on the stack, evaluating it again would loop
    # forever: it depends, possibly through other cells, on itself. Report
    # the whole loop, e.g. "A1 -> B1 -> A1".
    if @visiting.include?(name)
      chain = (@visiting + [name]).join(" -> ")
      raise CircularReferenceError.new("circular reference: #{chain}")
    end

    # Mark in progress, evaluate (this may recurse into other cells), unmark,
    # remember. An empty cell reads as "0". The pop is not in an `ensure`:
    # any error ends the whole run, so a half-unwound stack is never reused.
    @visiting.push(name)
    result = self.evaluate_raw(@raw.fetch(name, "0"))
    @visiting.pop()
    @cache[name] = result
    result
  end

  private

  # Classifies raw cell text, in this order: a formula ("=..."), a quoted
  # label ("\"...\""), a bare number, else a bare word kept as a label.
  def evaluate_raw(raw: String) -> Float | String
    text = raw.strip()

    if text.start_with?("=")
      self.eval_node(Parser.parse(text.slice(1, text.length() - 1)))
    elsif text.start_with?("\"") && text.end_with?("\"") && text.length() >= 2
      text.slice(1, text.length() - 2)
    elsif sheet_looks_numeric?(text)
      text.to_f()
    else
      text
    end
  end

  # Arithmetic coerces a text label to 0, the same forgiving direction
  # Ruby's own String#to_f takes on non-numeric text -- a real spreadsheet
  # would raise #VALUE!; this one keeps that one behavior simple and
  # documents it instead.
  def as_number(value: Float | String) -> Float
    if value is String then (sheet_looks_numeric?(value) ? value.to_f() : 0.0) else value end
  end

  # Evaluate one tree node. A RefNode calls back into value_of, which is
  # what walks the dependency graph (see spreadsheet.di's header).
  def eval_node(node: Node) -> Float | String
    case node
    when NumberNode then node.value()
    when RefNode then self.value_of(node.name())
    when NegNode then -self.as_number(self.eval_node(node.operand()))
    when BinOpNode then self.eval_binop(node)
    when CallNode then self.eval_call(node)
    end
  end

  # Both sides are coerced to numbers first, so "label" + 1 is 1.
  def eval_binop(node: BinOpNode) -> Float
    left = self.as_number(self.eval_node(node.left()))
    right = self.as_number(self.eval_node(node.right()))
    case node.op()
    when "+" then left + right
    when "-" then left - right
    when "*" then left * right
    when "/"
      raise FormulaError.new("division by zero", 0) if right == 0.0
      left / right
    end
  end

  # SUM/AVG/MIN/MAX over a rectangle. First every cell in the range is
  # evaluated (and coerced to a number), then the function folds the list.
  def eval_call(node: CallNode) -> Float
    values = cellref_range(node.from(), node.to()).map() do |name|
      self.as_number(self.value_of(name))
    end
    # `reduce(start) do |acc, v| ... end` folds the list. AVG's divisor sits
    # after the block's `end`: the block belongs to `reduce`, and the result
    # is then divided by the count (an empty range averages to 0).
    case node.name().upcase()
    when "SUM" then values.reduce(0.0) do |total, v| total + v end
    when "AVG" then values.empty?() ? 0.0 : values.reduce(0.0) do |total, v| total + v end / values.length()
    when "MIN" then values.reduce(values[0]) do |best, v| v < best ? v : best end
    when "MAX" then values.reduce(values[0]) do |best, v| v > best ? v : best end
    else raise FormulaError.new("unknown function #{node.name()}", 0)
    end
  end
end

# True for text like "12", "-3.5", ".5": an optional leading minus, then
# digits with at most one dot. Written by hand because it must be stricter
# than String#to_f, which would accept "12abc" as 12.
def sheet_looks_numeric?(text: String) -> Bool
  return false if text.empty?()

  # Skip a leading "-"; a lone "-" is not a number.
  index = text[0] == "-" ? 1 : 0
  return false if index >= text.length()

  seen_dot = false

  while index < text.length()
    char = text[index]
    if char == "."
      return false if seen_dot
      seen_dot = true
    elsif !formula_digit?(char)
      return false
    end
    index += 1
  end
  true
end
