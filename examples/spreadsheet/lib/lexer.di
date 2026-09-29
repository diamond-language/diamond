# Turns one formula's text (everything after the leading "=") into Tokens.
# A run of letters immediately followed by digits ("A1", "AA12") is a
# :ref; letters alone ("SUM") are a :name; digits alone (with at most one
# ".") are a :number.

struct Token(kind: Symbol, text: String, column: Int)
end

class FormulaError < StandardError
  def initialize(message, column)
    super(message)
    @column = column
  end
  def column() = @column
end

def formula_digit?(char: String) -> Bool
  code = char.ord()
  code >= 48 && code <= 57
end

def formula_letter?(char: String) -> Bool
  code = char.upcase().ord()
  code >= 65 && code <= 90
end

def formula_tokenize(text: String) -> Array[Token]
  tokens = []
  i = 0
  while i < text.length()
    char = text[i]
    start = i
    case
    when char == " " || char == "\t"
      i += 1
    when formula_digit?(char) || char == "."
      while i < text.length() && (formula_digit?(text[i]) || text[i] == ".")
        i += 1
      end
      tokens.push(Token.new(:number, text.slice(start, i - start), start))
    when formula_letter?(char)
      while i < text.length() && formula_letter?(text[i])
        i += 1
      end
      if i < text.length() && formula_digit?(text[i])
        while i < text.length() && formula_digit?(text[i])
          i += 1
        end
        tokens.push(Token.new(:ref, text.slice(start, i - start), start))
      else
        tokens.push(Token.new(:name, text.slice(start, i - start), start))
      end
    when "+-*/(),:".index_of(char) != nil
      tokens.push(Token.new(:op, char, start))
      i += 1
    else
      raise FormulaError.new("unexpected character '#{char}'", start)
    end
  end
  tokens.push(Token.new(:end, "", text.length()))
  tokens
end
