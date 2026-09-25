# Turns one input line into Tokens. Each token remembers its column so an
# error can point at the exact spot with a caret.

struct Token(kind: Symbol, text: String, column: Int)
end

# Every error the calculator reports carries the column it happened at.
class CalcError < StandardError
  def initialize(message, column)
    super(message)
    @column = column
  end
  def column() = @column
end

class ParseError < CalcError
end

class EvalError < CalcError
end

def calc_digit?(char: String) -> Bool
  code = char.ord()
  code >= 48 && code <= 57
end

def calc_letter?(char: String) -> Bool
  code = char.downcase().ord()
  (code >= 97 && code <= 122) || char == "_"
end

def calc_tokenize(line: String) -> Array[Token]
  tokens = []
  i = 0
  while i < line.length()
    char = line[i]
    start = i
    case
    when char == " " || char == "\t"
      i += 1
    when calc_digit?(char)
      while i < line.length() && (calc_digit?(line[i]) || line[i] == ".")
        i += 1
      end
      tokens.push(Token.new(:number, line.slice(start, i - start), start))
    when calc_letter?(char)
      while i < line.length() && (calc_letter?(line[i]) || calc_digit?(line[i]))
        i += 1
      end
      tokens.push(Token.new(:name, line.slice(start, i - start), start))
    when "+-*/%^(),=".index_of(char) != nil
      tokens.push(Token.new(:op, char, start))
      i += 1
    else
      raise ParseError.new("unexpected character '#{char}'", start)
    end
  end
  tokens.push(Token.new(:end, "", line.length()))
  tokens
end
