# Turns one formula's text (everything after the leading "=") into Tokens.
# A run of letters immediately followed by digits ("A1", "AA12") is a
# :ref; letters alone ("SUM") are a :name; digits alone (with at most one
# ".") are a :number.

struct Token(kind: Symbol, text: String, column: Int)
end

# Raised for any problem in a formula's text. `column` is the offset in the
# formula (0 when no better position is known).
class FormulaError < StandardError
  def initialize(message, column)
    super(message)
    @column = column
  end
  def column() = @column
end

# Character-class helpers, by character code (48-57 are "0"-"9", 65-90 are
# "A"-"Z" once uppercased).
def formula_digit?(char: String) -> Bool
  code = char.ord()
  code >= 48 && code <= 57
end

def formula_letter?(char: String) -> Bool
  code = char.upcase().ord()
  code >= 65 && code <= 90
end

# Scans left to right; `start` is where the current token began.
def formula_tokenize(text: String) -> Array[Token]
  tokens = []
  i = 0

  while i < text.length()
    char = text[i]
    start = i

    case
    # Whitespace separates tokens and produces none.
    when char == " " || char == "\t"
      i += 1
    # A number: digits and dots (a leading "." is allowed, so ".5" works).
    # More than one dot is accepted by the lexer and read leniently by to_f.
    when formula_digit?(char) || char == "."
      while i < text.length() && (formula_digit?(text[i]) || text[i] == ".")
        i += 1
      end
      tokens.push(Token.new(:number, text.slice(start, i - start), start))
    # Letters. Take the whole run first; what comes next decides what it is.
    when formula_letter?(char)
      while i < text.length() && formula_letter?(text[i])
        i += 1
      end

      # Digits right after the letters make a cell reference ("A1");
      # letters alone are a function name ("SUM").
      if i < text.length() && formula_digit?(text[i])
        while i < text.length() && formula_digit?(text[i])
          i += 1
        end
        tokens.push(Token.new(:ref, text.slice(start, i - start), start))
      else
        tokens.push(Token.new(:name, text.slice(start, i - start), start))
      end
    # Single-character operators and punctuation.
    when "+-*/(),:".index_of(char) != nil
      tokens.push(Token.new(:op, char, start))
      i += 1
    else
      raise FormulaError.new("unexpected character '#{char}'", start)
    end
  end

  # Sentinel end token, so the parser can always peek one more token.
  tokens.push(Token.new(:end, "", text.length()))
  tokens
end
