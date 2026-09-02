class JSONError < StandardError
  attr_reader message: String
  def initialize(message: String)
    @message = message
  end
end

# All mutually-recursive parse/stringify steps live as methods on one
# class and call each other through `self.` -- a bare no-receiver call
# resolves against locals/top-level functions at compile time in file
# order (no forward references), but `self.name(...)` dispatches by
# name at call time regardless of declaration order, which is what a
# recursive-descent JSON parser (value <-> array <-> object) and a
# recursive stringifier (stringify <-> stringify_array/stringify_hash)
# both need.
class JSONCodec
  def is_digit(ch: String) -> Bool
    code = ch.ord()
    code >= 48 && code <= 57
  end

  def is_whitespace(ch: String) -> Bool
    ch == " " || ch == "\t" || ch == "\n" || ch == "\r"
  end

  def hex_digit_value(ch: String) -> Int
    code = ch.ord()
    if code >= 48 && code <= 57
      code - 48
    elsif code >= 97 && code <= 102
      code - 97 + 10
    elsif code >= 65 && code <= 70
      code - 65 + 10
    else
      raise JSONError.new("invalid unicode escape hex digit")
    end
  end

  def parse_hex4(source: String, pos: Int) -> Int
    value = 0
    index = 0
    while index < 4
      value = value * 16 + self.hex_digit_value(source[pos + index])
      index += 1
    end
    value
  end

  # A lone surrogate codepoint (0xD800-0xDFFF) reaching here is always
  # invalid -- real astral characters (emoji, etc.) are combined into a
  # single codepoint >= 0x10000 by parse_unicode_escape below *before*
  # utf8_encode ever sees them, so this only fires for a genuinely
  # malformed/unpaired \uXXXX escape.
  def utf8_encode(codepoint: Int) -> String
    if codepoint >= 55296 && codepoint <= 57343
      raise JSONError.new("lone surrogate codepoint is not valid UTF-8")
    elsif codepoint < 128
      codepoint.chr()
    elsif codepoint < 2048
      byte1 = 192 + codepoint / 64
      byte2 = 128 + mod(codepoint, 64)
      byte1.chr() + byte2.chr()
    elsif codepoint < 65536
      byte1 = 224 + codepoint / 4096
      byte2 = 128 + mod(codepoint / 64, 64)
      byte3 = 128 + mod(codepoint, 64)
      byte1.chr() + byte2.chr() + byte3.chr()
    else
      byte1 = 240 + codepoint / 262144
      byte2 = 128 + mod(codepoint / 4096, 64)
      byte3 = 128 + mod(codepoint / 64, 64)
      byte4 = 128 + mod(codepoint, 64)
      byte1.chr() + byte2.chr() + byte3.chr() + byte4.chr()
    end
  end

  def skip_whitespace(source: String, pos: Int) -> Int
    length = source.length()
    while pos < length && self.is_whitespace(source[pos])
      pos += 1
    end
    pos
  end

  def parse_literal(source: String, pos: Int, literal: String, value) -> Array
    length = literal.length()
    if pos + length > source.length() || source.slice(pos, length) != literal
      raise JSONError.new("invalid literal at position #{pos}")
    end
    [value, pos + length]
  end

  def parse_number(source: String, pos: Int) -> Array
    start = pos
    length = source.length()
    if pos < length && source[pos] == "-"
      pos += 1
    end
    digit_start = pos
    while pos < length && self.is_digit(source[pos])
      pos += 1
    end
    if pos == digit_start
      raise JSONError.new("invalid number at position #{start}")
    end
    is_float = false
    if pos < length && source[pos] == "."
      is_float = true
      pos += 1
      fraction_start = pos
      while pos < length && self.is_digit(source[pos])
        pos += 1
      end
      if pos == fraction_start
        raise JSONError.new("invalid number at position #{start}")
      end
    end
    if pos < length && (source[pos] == "e" || source[pos] == "E")
      is_float = true
      pos += 1
      if pos < length && (source[pos] == "+" || source[pos] == "-")
        pos += 1
      end
      exponent_start = pos
      while pos < length && self.is_digit(source[pos])
        pos += 1
      end
      if pos == exponent_start
        raise JSONError.new("invalid number at position #{start}")
      end
    end
    text = source.slice(start, pos - start)
    value = if is_float
      text.to_f()
    else
      text.to_i()
    end
    [value, pos]
  end

  # `pos` points at the "u" of a \uXXXX escape. A high surrogate
  # (0xD800-0xDBFF) must be immediately followed by a second \uXXXX
  # escape holding a low surrogate (0xDC00-0xDFFF) -- real JSON
  # encodes an astral character (outside the BMP, e.g. emoji) as
  # exactly that pair, per RFC 8259 -- combined here into the single
  # codepoint >= 0x10000 the pair represents before handing it to
  # utf8_encode. Returns [encoded_string, pos_of_last_consumed_char],
  # matching parse_hex4's own "still pointing at the last digit"
  # convention so the caller's shared `pos += 1` keeps working
  # unchanged for both the single- and paired-escape cases.
  def parse_unicode_escape(source: String, pos: Int) -> Array
    length = source.length()
    if pos + 4 >= length
      raise JSONError.new("truncated unicode escape")
    end
    code = self.parse_hex4(source, pos + 1)
    end_pos = pos + 4
    if code >= 55296 && code <= 56319
      if end_pos + 6 >= length || source[end_pos + 1] != "\\" || source[end_pos + 2] != "u"
        raise JSONError.new("unpaired high surrogate in unicode escape")
      end
      low = self.parse_hex4(source, end_pos + 3)
      if low < 56320 || low > 57343
        raise JSONError.new("high surrogate not followed by a low surrogate in unicode escape")
      end
      codepoint = 65536 + (code - 55296) * 1024 + (low - 56320)
      [self.utf8_encode(codepoint), end_pos + 6]
    elsif code >= 56320 && code <= 57343
      raise JSONError.new("unpaired low surrogate in unicode escape")
    else
      [self.utf8_encode(code), end_pos]
    end
  end

  def parse_string(source: String, pos: Int) -> Array
    pos += 1
    length = source.length()
    result = ""
    while true
      if pos >= length
        raise JSONError.new("unterminated string")
      end
      ch = source[pos]
      if ch == "\""
        return [result, pos + 1]
      elsif ch == "\\"
        pos += 1
        if pos >= length
          raise JSONError.new("unterminated escape sequence")
        end
        escape = source[pos]
        if escape == "\""
          result = result + "\""
        elsif escape == "\\"
          result = result + "\\"
        elsif escape == "/"
          result = result + "/"
        elsif escape == "n"
          result = result + "\n"
        elsif escape == "r"
          result = result + "\r"
        elsif escape == "t"
          result = result + "\t"
        elsif escape == "b"
          result = result + 8.chr()
        elsif escape == "f"
          result = result + 12.chr()
        elsif escape == "u"
          escape_result = self.parse_unicode_escape(source, pos)
          result = result + escape_result[0]
          pos = escape_result[1]
        else
          raise JSONError.new("invalid escape character")
        end
        pos += 1
      else
        result = result + ch
        pos += 1
      end
    end
  end

  def parse_array(source: String, pos: Int) -> Array
    pos += 1
    pos = self.skip_whitespace(source, pos)
    result = []
    if pos < source.length() && source[pos] == "]"
      return [result, pos + 1]
    end
    while true
      parsed = self.parse_value(source, pos)
      result.push(parsed[0])
      pos = self.skip_whitespace(source, parsed[1])
      if pos >= source.length()
        raise JSONError.new("unterminated array")
      end
      if source[pos] == ","
        pos = self.skip_whitespace(source, pos + 1)
      elsif source[pos] == "]"
        return [result, pos + 1]
      else
        raise JSONError.new("expected ',' or ']' in array")
      end
    end
  end

  def parse_object(source: String, pos: Int) -> Array
    pos += 1
    pos = self.skip_whitespace(source, pos)
    result = {}
    if pos < source.length() && source[pos] == "}"
      return [result, pos + 1]
    end
    while true
      pos = self.skip_whitespace(source, pos)
      if pos >= source.length() || source[pos] != "\""
        raise JSONError.new("expected string key in object")
      end
      key_result = self.parse_string(source, pos)
      pos = self.skip_whitespace(source, key_result[1])
      if pos >= source.length() || source[pos] != ":"
        raise JSONError.new("expected ':' after object key")
      end
      pos = self.skip_whitespace(source, pos + 1)
      value_result = self.parse_value(source, pos)
      result[key_result[0]] = value_result[0]
      pos = self.skip_whitespace(source, value_result[1])
      if pos >= source.length()
        raise JSONError.new("unterminated object")
      end
      if source[pos] == ","
        pos += 1
      elsif source[pos] == "}"
        return [result, pos + 1]
      else
        raise JSONError.new("expected ',' or '}' in object")
      end
    end
  end

  def parse_value(source: String, pos: Int) -> Array
    pos = self.skip_whitespace(source, pos)
    if pos >= source.length()
      raise JSONError.new("unexpected end of input")
    end
    ch = source[pos]
    if ch == "{"
      self.parse_object(source, pos)
    elsif ch == "["
      self.parse_array(source, pos)
    elsif ch == "\""
      self.parse_string(source, pos)
    elsif ch == "t"
      self.parse_literal(source, pos, "true", true)
    elsif ch == "f"
      self.parse_literal(source, pos, "false", false)
    elsif ch == "n"
      self.parse_literal(source, pos, "null", nil)
    elsif ch == "-" || self.is_digit(ch)
      self.parse_number(source, pos)
    else
      raise JSONError.new("unexpected character at position #{pos}")
    end
  end

  def parse(source: String)
    parsed = self.parse_value(source, 0)
    pos = self.skip_whitespace(source, parsed[1])
    if pos != source.length()
      raise JSONError.new("trailing content after JSON value")
    end
    parsed[0]
  end

  def hex_digit(value: Int) -> String
    if value < 10
      "#{value}"
    else
      (97 + value - 10).chr()
    end
  end

  def escape_control(code: Int) -> String
    high = code / 16
    low = mod(code, 16)
    "\\u00" + self.hex_digit(high) + self.hex_digit(low)
  end

  def stringify_string(value: String) -> String
    result = "\""
    index = 0
    length = value.length()
    while index < length
      ch = value[index]
      if ch == "\""
        result = result + "\\\""
      elsif ch == "\\"
        result = result + "\\\\"
      elsif ch == "\n"
        result = result + "\\n"
      elsif ch == "\r"
        result = result + "\\r"
      elsif ch == "\t"
        result = result + "\\t"
      elsif ch.ord() < 32
        result = result + self.escape_control(ch.ord())
      else
        result = result + ch
      end
      index += 1
    end
    result + "\""
  end

  def stringify_array(value: Array) -> String
    result = "["
    index = 0
    length = value.length()
    while index < length
      if index > 0
        result = result + ","
      end
      result = result + self.stringify(value[index])
      index += 1
    end
    result + "]"
  end

  def stringify_hash(value: Hash) -> String
    result = "{"
    index = 0
    length = value.length()
    while index < length
      if index > 0
        result = result + ","
      end
      key = value.key_at(index)
      key_string = if key is String
        key
      else
        "#{key}"
      end
      encoded_value = self.stringify(value.value_at(index))
      result = result + self.stringify_string(key_string) + ":" + encoded_value
      index += 1
    end
    result + "}"
  end

  def stringify(value) -> String
    if value == nil
      "null"
    elsif value is Bool
      if value
        "true"
      else
        "false"
      end
    elsif value is Int
      "#{value}"
    elsif value is Float
      "#{value}"
    elsif value is String
      self.stringify_string(value)
    elsif value is Array
      self.stringify_array(value)
    elsif value is Hash
      self.stringify_hash(value)
    else
      raise JSONError.new("cannot convert this value to JSON")
    end
  end
end



