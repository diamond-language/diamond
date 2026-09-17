# Parsing lives natively now (String#parse_json, src/vm.c) -- see that
# function's own comment for why (~330ms/MB in this pure-Diamond form,
# measured directly against a real training-corpus-scale dataset) and
# for the exact grammar/error-contract parity this replaced. JSONError
# itself is a builtin now too (DIAMOND_CLASS_JSON_ERROR), so native code
# can raise it the same way every other native error class already does
# -- no class declaration needed here anymore.
#
# Stringification stays pure Diamond: no comparable performance problem
# was ever measured for it, and JSONCodec's own mutual recursion here
# (stringify <-> stringify_array/stringify_hash, all through `self.` for
# call-time dispatch regardless of declaration order) still needs
# nothing native to do its job.
class JSONCodec
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
