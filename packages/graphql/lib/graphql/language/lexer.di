# Hand-rolled scanner over query-document source text -- no Regexp
# (Diamond's Regexp has no String integration, see packages/div's own
# compiler.di for the established precedent of index_of/slice-based
# scanning instead), character classification done via String#ord
# range checks (see packages/active_record's
# Transaction.validate_savepoint_name for that same pattern).
#
# Query/mutation/subscription/fragment/on/true/false/null are NOT
# recognized as distinct token kinds here -- GraphQL's own grammar
# treats them as ordinary Name tokens that the parser checks
# contextually (they're not reserved words; a field can legally be
# named `query`). Keeping that distinction out of the lexer entirely
# matches the spec.
module GraphQL
module Language

class Token
  attr_reader kind: String
  attr_reader value: String
  attr_reader line: Int
  def initialize(kind: String, value: String, line: Int)
    @kind = kind
    @value = value
    @line = line
  end
end

class LexError < StandardError
  attr_reader message: String
  attr_reader line: Int
  def initialize(message: String, line: Int)
    @message = "#{message} (line #{line})"
    @line = line
  end
end

class Lexer
  # Tokenizes `source` into an Array of Token, ending with one "EOF"
  # token so callers never need a separate end-of-input check.
  def self.tokenize(source: String)
    tokens = []
    length = source.length()
    pos = 0
    line = 1
    while pos < length
      ch = source[pos]
      if ch == " " || ch == "\t" || ch == "\r" || ch == ","
        pos += 1
      elsif ch == "\n"
        line += 1
        pos += 1
      elsif ch == "#"
        while pos < length && source[pos] != "\n"
          pos += 1
        end
      elsif ch == "\""
        [value, next_pos, line_delta] = self.scan_string(source, pos, line)
        tokens.push(Token.new("STRING", value, line))
        pos = next_pos
        line = line + line_delta
      elsif self.is_name_start(ch)
        [value, next_pos] = self.scan_name(source, pos)
        tokens.push(Token.new("NAME", value, line))
        pos = next_pos
      elsif self.is_digit(ch) || (ch == "-" && pos + 1 < length && self.is_digit(source[pos + 1]))
        [kind, value, next_pos] = self.scan_number(source, pos, line)
        tokens.push(Token.new(kind, value, line))
        pos = next_pos
      elsif ch == "." && pos + 2 < length && source[pos + 1] == "." && source[pos + 2] == "."
        tokens.push(Token.new("PUNCT", "...", line))
        pos += 3
      elsif self.is_punct(ch)
        tokens.push(Token.new("PUNCT", ch, line))
        pos += 1
      else
        raise LexError.new("unexpected character #{ch}", line)
      end
    end
    tokens.push(Token.new("EOF", "", line))
    tokens
  end

  def self.is_digit(ch)
    code = ch.ord()
    code >= 48 && code <= 57
  end

  def self.is_name_start(ch)
    code = ch.ord()
    (code >= 65 && code <= 90) || (code >= 97 && code <= 122) || code == 95
  end

  def self.is_name_char(ch)
    self.is_name_start(ch) || self.is_digit(ch)
  end

  def self.is_punct(ch)
    ch == "!" || ch == "$" || ch == "(" || ch == ")" || ch == ":" ||
      ch == "=" || ch == "@" || ch == "[" || ch == "]" || ch == "{" ||
      ch == "|" || ch == "}"
  end

  # [name_text, next_pos]
  def self.scan_name(source, start)
    length = source.length()
    pos = start
    while pos < length && self.is_name_char(source[pos])
      pos += 1
    end
    [source.slice(start, pos - start), pos]
  end

  # [kind ("INT" or "FLOAT"), text, next_pos] -- doesn't enforce the
  # spec's "no leading zero" rule (e.g. `007`), a minor validation
  # nicety left out for v1, see ROADMAP.md.
  def self.scan_number(source, start, line)
    length = source.length()
    pos = start
    if source[pos] == "-"
      pos += 1
    end
    unless pos < length && self.is_digit(source[pos])
      raise LexError.new("expected a digit after '-'", line)
    end
    while pos < length && self.is_digit(source[pos])
      pos += 1
    end
    is_float = false
    if pos < length && source[pos] == "." && pos + 1 < length && self.is_digit(source[pos + 1])
      is_float = true
      pos += 1
      while pos < length && self.is_digit(source[pos])
        pos += 1
      end
    end
    if pos < length && (source[pos] == "e" || source[pos] == "E")
      exp_pos = pos + 1
      if exp_pos < length && (source[exp_pos] == "+" || source[exp_pos] == "-")
        exp_pos += 1
      end
      if exp_pos < length && self.is_digit(source[exp_pos])
        is_float = true
        pos = exp_pos
        while pos < length && self.is_digit(source[pos])
          pos += 1
        end
      end
    end
    kind = if is_float then "FLOAT" else "INT" end
    [kind, source.slice(start, pos - start), pos]
  end

  # [decoded_text, next_pos, newline_count] -- dispatches to a plain
  # quoted string or a triple-quoted block string.
  def self.scan_string(source, start, line)
    if start + 3 <= source.length() && source.slice(start, 3) == "\"\"\""
      self.scan_block_string(source, start, line)
    else
      result = self.scan_quoted_string(source, start, line)
      [result[0], result[1], 0]
    end
  end

  # [decoded_text, next_pos] -- a single-line quoted string. Supports
  # the common escapes (\" \\ \/ \n \r \t); \b, \f, and \uXXXX raise a
  # LexError for v1 (no codepoint-to-character conversion available to
  # decode \uXXXX into a real Diamond String, and \b/\f aren't legal
  # Diamond string-literal escapes to build with either -- see
  # ROADMAP.md).
  def self.scan_quoted_string(source, start, line)
    length = source.length()
    pos = start + 1
    sb = StringBuilder.new()
    loop do
      if pos >= length
        raise LexError.new("unterminated string", line)
      end
      ch = source[pos]
      if ch == "\""
        pos += 1
        break
      elsif ch == "\n"
        raise LexError.new("unterminated string (newline in a non-block string)", line)
      elsif ch == "\\"
        pos += 1
        if pos >= length
          raise LexError.new("unterminated string", line)
        end
        esc = source[pos]
        if esc == "\""
          sb.append("\"")
        elsif esc == "\\"
          sb.append("\\")
        elsif esc == "/"
          sb.append("/")
        elsif esc == "n"
          sb.append("\n")
        elsif esc == "r"
          sb.append("\r")
        elsif esc == "t"
          sb.append("\t")
        else
          raise LexError.new("unsupported escape sequence \\#{esc}", line)
        end
        pos += 1
      else
        sb.append(ch)
        pos += 1
      end
    end
    [sb.to_s(), pos]
  end

  # [dedented_text, next_pos, newline_count] -- collects raw content up
  # to the closing `"""` (a `\"""` sequence inside the string is a
  # literal `"""`, per spec, not the terminator), then runs the same
  # dedent algorithm as graphql-ruby's own Language::BlockString.
  def self.scan_block_string(source, start, line)
    length = source.length()
    pos = start + 3
    raw = StringBuilder.new()
    newlines = 0
    loop do
      if pos >= length
        raise LexError.new("unterminated block string", line)
      end
      if pos + 3 <= length && source.slice(pos, 3) == "\"\"\""
        pos += 3
        break
      elsif pos + 4 <= length && source.slice(pos, 4) == "\\\"\"\""
        raw.append("\"\"\"")
        pos += 4
      else
        ch = source[pos]
        if ch == "\n"
          newlines += 1
        end
        raw.append(ch)
        pos += 1
      end
    end
    [self.dedent_block_string(raw.to_s()), pos, newlines]
  end

  # Removes the common leading whitespace from every line but the
  # first, then trims leading/trailing blank lines -- see "Block
  # Strings" in the GraphQL spec's Language section.
  def self.dedent_block_string(text)
    if text == ""
      return text
    end
    lines = text.split("\n")
    common_indent = nil
    i = 1
    while i < lines.length()
      line_text = lines[i]
      leading = self.leading_spaces(line_text)
      if leading < line_text.length() && (common_indent == nil || leading < common_indent)
        common_indent = leading
      end
      i += 1
    end
    if common_indent != nil && common_indent > 0
      i = 1
      while i < lines.length()
        lines[i] = lines[i].slice(common_indent, lines[i].length() - common_indent)
        i += 1
      end
    end
    while lines.length() > 0 && self.blank?(lines[0])
      lines.delete_at(0)
    end
    while lines.length() > 0 && self.blank?(lines[lines.length() - 1])
      lines.pop()
    end
    lines.join("\n")
  end

  def self.leading_spaces(text)
    i = 0
    while i < text.length() && text[i] == " "
      i += 1
    end
    i
  end

  def self.blank?(text)
    self.leading_spaces(text) == text.length()
  end
end

end
end
