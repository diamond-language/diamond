# Parsing lives natively now (String#parse_json, src/vm.c) -- see that
# function's own comment for why (~330ms/MB in this pure-Diamond form,
# measured directly against a real training-corpus-scale dataset) and
# for the exact grammar/error-contract parity this replaced. JSONError
# itself is a builtin now too (DIAMOND_CLASS_JSON_ERROR), so native code
# can raise it the same way every other native error class already does
# -- no class declaration needed here anymore.
#
# Stringification stays pure Diamond, writing into one StringBuilder so it
# stays linear (it used to concatenate Strings, which was quadratic: a
# 200 KB string took 2.4s). JSONCodec's mutual recursion (write_value <->
# write_array/write_hash) goes through `self.` for call-time dispatch
# regardless of declaration order.
class JSONCodec
  def initialize()
    # A character JSON requires escaping: a quote, a backslash, or a
    # control character.
    @needs_escape = Regexp.new("[\"\\\\\\x00-\\x1f]")
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

  # Output is built in one StringBuilder, so the cost is linear in the
  # size of the document -- `result = result + piece` would copy the whole
  # result on every append.
  def stringify(value) -> String
    out = StringBuilder.new()
    self.write_value(out, value)
    out.to_s()
  end

  def write_value(out, value)
    if value == nil
      out.append("null")
    elsif value is Bool
      out.append(if value then "true" else "false" end)
    elsif value is Int
      out.append("#{value}")
    elsif value is Float
      # JSON has no NaN or Infinity.
      if value != value || value * 0.0 != 0.0
        raise JSONError.new("cannot convert #{value} to JSON")
      end
      out.append("#{value}")
    elsif value is String
      self.write_string(out, value)
    elsif value is Array
      self.write_array(out, value)
    elsif value is Hash
      self.write_hash(out, value)
    else
      raise JSONError.new("cannot convert this value to JSON")
    end
  end

  def write_string(out, value: String)
    out.append("\"")
    # Most strings need no escaping; append those whole.
    unless @needs_escape.match?(value)
      out.append(value)
      out.append("\"")
      return
    end
    index = 0
    length = value.length()
    while index < length
      ch = value[index]
      if ch == "\""
        out.append("\\\"")
      elsif ch == "\\"
        out.append("\\\\")
      elsif ch == "\n"
        out.append("\\n")
      elsif ch == "\r"
        out.append("\\r")
      elsif ch == "\t"
        out.append("\\t")
      elsif ch.ord() < 32
        out.append(self.escape_control(ch.ord()))
      else
        out.append(ch)
      end
      index += 1
    end
    out.append("\"")
  end

  def write_array(out, value: Array)
    out.append("[")
    index = 0
    length = value.length()
    while index < length
      out.append(",") if index > 0
      self.write_value(out, value[index])
      index += 1
    end
    out.append("]")
  end

  def write_hash(out, value: Hash)
    out.append("{")
    index = 0
    length = value.length()
    while index < length
      out.append(",") if index > 0
      key = value.key_at(index)
      self.write_string(out, if key is String then key else "#{key}" end)
      out.append(":")
      self.write_value(out, value.value_at(index))
      index += 1
    end
    out.append("}")
  end
end
