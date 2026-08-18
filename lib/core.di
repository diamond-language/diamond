# This file is compiled as ordinary Diamond before every user program.

def array_first(values: Array)
  values[0]
end

def array_first_or(values: Array, fallback)
  if values.length() == 0
    fallback
  else
    values[0]
  end
end

def array_last(values: Array)
  values[values.length() - 1]
end

def array_last_or(values: Array, fallback)
  if values.length() == 0
    fallback
  else
    array_last(values)
  end
end

def array_empty(values: Array) -> Bool
  values.length() == 0
end

def array_include(values: Array, needle) -> Bool
  index = 0
  while index < values.length()
    if values[index] == needle
      return true
    end
    index = index + 1
  end
  false
end

def array_each(values: Array, callback: Callable[1]) -> Array
  index = 0
  while index < values.length()
    callback(values[index])
    index = index + 1
  end
  values
end

def array_map(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index = index + 1
  end
  result
end

def array_map_int(values: Array, callback: Callable[1, Int]) -> Array[Int]
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index = index + 1
  end
  result
end

def array_map_string(values: Array, callback: Callable[1, String]) -> Array[String]
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index = index + 1
  end
  result
end

def array_map_typed[T, U](values: Array[T], callback: Callable[[T], U]) -> Array[U]
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index = index + 1
  end
  result
end

def array_swap_first_two(values: Array) -> Array
  first = values[0]
  second = values[1]
  values[0] = second
  values[1] = first
  values
end

def array_reverse(values: Array) -> Array
  result = []
  index = values.length() - 1
  while index >= 0
    result.push(values[index])
    index = index - 1
  end
  result
end

def array_concat(values: Array, other: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index = index + 1
  end
  index = 0
  while index < other.length()
    result.push(other[index])
    index = index + 1
  end
  result
end

def array_compact(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    if item != nil
      result.push(item)
    end
    index = index + 1
  end
  result
end

def array_uniq(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    if array_include(result, item) == false
      result.push(item)
    end
    index = index + 1
  end
  result
end

def array_flatten(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    if item is Array
      result = array_concat(result, array_flatten(item))
    else
      result.push(item)
    end
    index = index + 1
  end
  result
end

# Kept as a free function for existing callers, but the loop that used
# to live here (`result = result + "#{piece}"`, once per element) was
# exactly the O(n^2) concatenation-in-a-loop pattern the pre-release
# audit flagged -- #join is now a genuine native, StringBuilder-backed
# method (src/vm.c's INVOKE handler), O(n) total.
def array_join(values: Array, separator: String = "") -> String
  values.join(separator)
end

def array_delete_at(values: Array, index: Int)
  length = values.length()
  if index < 0 || index >= length
    nil
  else
    removed = values[index]
    shift = index
    while shift < length - 1
      values[shift] = values[shift + 1]
      shift = shift + 1
    end
    values.pop()
    removed
  end
end

def hash_fetch(values: Hash, key, fallback)
  found = values[key]
  if found == nil
    fallback
  else
    found
  end
end


def hash_empty(values: Hash) -> Bool
  values.length() == 0
end

def hash_each(values: Hash, callback: Callable[2]) -> Hash
  index = 0
  count = values.length()
  while index < count
    callback(values.key_at(index), values.value_at(index))
    index = index + 1
  end
  values
end

def hash_keys(values: Hash) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values.key_at(index))
    index = index + 1
  end
  result
end

def hash_values(values: Hash) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values.value_at(index))
    index = index + 1
  end
  result
end

def hash_include_key(values: Hash, needle) -> Bool
  index = 0
  while index < values.length()
    if values.key_at(index) == needle
      return true
    end
    index = index + 1
  end
  false
end

def hash_map_values(values: Hash, callback: Callable[1]) -> Hash
  result = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    result[key] = callback(values.value_at(index))
    index = index + 1
  end
  result
end

def hash_merge(a: Hash, b: Hash) -> Hash
  result = {}
  index = 0
  while index < a.length()
    result[a.key_at(index)] = a.value_at(index)
    index = index + 1
  end
  index = 0
  while index < b.length()
    result[b.key_at(index)] = b.value_at(index)
    index = index + 1
  end
  result
end

def enumerable_select(values, callback: Callable[1]) -> Array
  result = []
  if values is Hash
    def collect_pair(key, value)
      if callback(value)
        result.push(value)
      end
    end
    values.each(collect_pair)
  else
    def collect_item(item)
      if callback(item)
        result.push(item)
      end
    end
    values.each(collect_item)
  end
  result
end

def enumerable_count(values, callback: Callable[1]) -> Int
  total = 0
  if values is Hash
    def tally_pair(key, value)
      if callback(value)
        total = total + 1
      end
    end
    values.each(tally_pair)
  else
    def tally_item(item)
      if callback(item)
        total = total + 1
      end
    end
    values.each(tally_item)
  end
  total
end

def enumerable_any(values, callback: Callable[1]) -> Bool
  found = false
  if values is Hash
    def probe_pair(key, value)
      if callback(value)
        found = true
      end
    end
    values.each(probe_pair)
  else
    def probe_item(item)
      if callback(item)
        found = true
      end
    end
    values.each(probe_item)
  end
  found
end

def enumerable_all(values, callback: Callable[1]) -> Bool
  result = true
  if values is Hash
    def check_pair(key, value)
      if callback(value) == false
        result = false
      end
    end
    values.each(check_pair)
  else
    def check_item(item)
      if callback(item) == false
        result = false
      end
    end
    values.each(check_item)
  end
  result
end

def enumerable_map(values, callback: Callable[1]) -> Array
  result = []
  if values is Hash
    def transform_pair(key, value)
      result.push(callback(value))
    end
    values.each(transform_pair)
  else
    def transform_item(item)
      result.push(callback(item))
    end
    values.each(transform_item)
  end
  result
end

def enumerable_reduce(values, initial, callback: Callable[2])
  accumulator = initial
  if values is Hash
    def combine_pair(key, value)
      accumulator = callback(accumulator, value)
    end
    values.each(combine_pair)
  else
    def combine_item(item)
      accumulator = callback(accumulator, item)
    end
    values.each(combine_item)
  end
  accumulator
end

def array_sum(values: Array)
  total = 0
  index = 0
  while index < values.length()
    total = total + values[index]
    index = index + 1
  end
  total
end

def array_reject(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    if callback(item) == false
      result.push(item)
    end
    index = index + 1
  end
  result
end

def array_find(values: Array, callback: Callable[1])
  index = 0
  while index < values.length()
    item = values[index]
    if callback(item)
      return item
    end
    index = index + 1
  end
  nil
end

def array_each_with_index(values: Array, callback: Callable[2]) -> Array
  index = 0
  while index < values.length()
    callback(values[index], index)
    index = index + 1
  end
  values
end

def enumerable_sort(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index = index + 1
  end
  i = 1
  while i < result.length()
    key = result[i]
    j = i - 1
    while j >= 0 && result[j] > key
      result[j + 1] = result[j]
      j = j - 1
    end
    result[j + 1] = key
    i = i + 1
  end
  result
end

def enumerable_sort_by(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index = index + 1
  end
  i = 1
  while i < result.length()
    key = result[i]
    key_value = callback(key)
    j = i - 1
    while j >= 0 && callback(result[j]) > key_value
      result[j + 1] = result[j]
      j = j - 1
    end
    result[j + 1] = key
    i = i + 1
  end
  result
end

def enumerable_min(values: Array)
  result = values[0]
  index = 1
  while index < values.length()
    if values[index] < result
      result = values[index]
    end
    index = index + 1
  end
  result
end

def enumerable_max(values: Array)
  result = values[0]
  index = 1
  while index < values.length()
    if values[index] > result
      result = values[index]
    end
    index = index + 1
  end
  result
end

module Enumerable
  def select(callback: Callable[1]) -> Array = enumerable_select(self, callback)
  def count(callback: Callable[1]) -> Int = enumerable_count(self, callback)
  def any?(callback: Callable[1]) -> Bool = enumerable_any(self, callback)
  def all?(callback: Callable[1]) -> Bool = enumerable_all(self, callback)
  def map(callback: Callable[1]) -> Array = enumerable_map(self, callback)
  def reduce(initial, callback: Callable[2]) = enumerable_reduce(self, initial, callback)
end

# `1..5` (inclusive) / `1...5` (exclusive) desugar directly to
# `Range.new(1, 5, false)` / `Range.new(1, 5, true)` at parse time (see
# compiler.c's range-desugaring branch in parse_precedence) -- Range
# itself is a plain class here, not a native object, same precedent as
# StringBuilder. `end` is a reserved keyword, hence `end_value`/`@end`.
class Range
  include Enumerable

  def initialize(start: Int, end_value: Int, exclusive: Bool)
    @start = start
    @end = end_value
    @exclusive = exclusive
  end

  def first() -> Int = @start
  def last() -> Int = @end
  def exclusive?() -> Bool = @exclusive

  def length() -> Int
    n = @end - @start
    n = n + 1 unless @exclusive
    n = 0 if n < 0
    n
  end

  def include?(value: Int) -> Bool
    within_end = if @exclusive
      value < @end
    else
      value <= @end
    end
    value >= @start && within_end
  end

  def each(callback: Callable[1])
    i = @start
    stop = if @exclusive
      @end
    else
      @end + 1
    end
    while i < stop
      callback(i)
      i = i + 1
    end
    self
  end
end

def abs(x: Int | Float) -> Int | Float
  if x < 0
    -x
  else
    x
  end
end

def min(a: Int | Float, b: Int | Float) -> Int | Float
  if a < b
    a
  else
    b
  end
end

def max(a: Int | Float, b: Int | Float) -> Int | Float
  if a > b
    a
  else
    b
  end
end

def mod(a: Int | Float, b: Int | Float) -> Int | Float
  quotient = a / b
  if quotient is Int
    a - quotient * b
  else
    a - to_f(to_i(quotient)) * b
  end
end

def array_sort(values: Array[Int]) -> Array[Int]
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index = index + 1
  end
  i = 1
  while i < result.length()
    key = result[i]
    j = i - 1
    while j >= 0 && result[j] > key
      result[j + 1] = result[j]
      j = j - 1
    end
    result[j + 1] = key
    i = i + 1
  end
  result
end

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
      index = index + 1
    end
    value
  end

  # BMP-only (0-0xFFFF, the full range four hex digits can express).
  # Surrogate pairs (astral characters split across two \uXXXX escapes)
  # are a deliberate scope cut -- see docs/roadmap.md.
  def utf8_encode(codepoint: Int) -> String
    if codepoint >= 55296 && codepoint <= 57343
      raise JSONError.new("surrogate pair unicode escapes are not supported")
    elsif codepoint < 128
      codepoint.chr()
    elsif codepoint < 2048
      byte1 = 192 + codepoint / 64
      byte2 = 128 + mod(codepoint, 64)
      byte1.chr() + byte2.chr()
    else
      byte1 = 224 + codepoint / 4096
      byte2 = 128 + mod(codepoint / 64, 64)
      byte3 = 128 + mod(codepoint, 64)
      byte1.chr() + byte2.chr() + byte3.chr()
    end
  end

  def skip_whitespace(source: String, pos: Int) -> Int
    length = source.length()
    while pos < length && self.is_whitespace(source[pos])
      pos = pos + 1
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
      pos = pos + 1
    end
    digit_start = pos
    while pos < length && self.is_digit(source[pos])
      pos = pos + 1
    end
    if pos == digit_start
      raise JSONError.new("invalid number at position #{start}")
    end
    is_float = false
    if pos < length && source[pos] == "."
      is_float = true
      pos = pos + 1
      fraction_start = pos
      while pos < length && self.is_digit(source[pos])
        pos = pos + 1
      end
      if pos == fraction_start
        raise JSONError.new("invalid number at position #{start}")
      end
    end
    if pos < length && (source[pos] == "e" || source[pos] == "E")
      is_float = true
      pos = pos + 1
      if pos < length && (source[pos] == "+" || source[pos] == "-")
        pos = pos + 1
      end
      exponent_start = pos
      while pos < length && self.is_digit(source[pos])
        pos = pos + 1
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

  def parse_string(source: String, pos: Int) -> Array
    pos = pos + 1
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
        pos = pos + 1
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
          if pos + 4 >= length
            raise JSONError.new("truncated unicode escape")
          end
          result = result + self.utf8_encode(self.parse_hex4(source, pos + 1))
          pos = pos + 4
        else
          raise JSONError.new("invalid escape character")
        end
        pos = pos + 1
      else
        result = result + ch
        pos = pos + 1
      end
    end
  end

  def parse_array(source: String, pos: Int) -> Array
    pos = pos + 1
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
    pos = pos + 1
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
        pos = pos + 1
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
      index = index + 1
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
      index = index + 1
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
      index = index + 1
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

module JSON
  module_function

  def stringify(value) -> String = JSONCodec.new().stringify(value)
  def parse(source: String) = JSONCodec.new().parse(source)
end

# A named escape hatch from `result = result + piece` in a loop -- the
# O(n^2) concatenation pattern the pre-release audit flagged, and the
# same reason #join above is a genuine native, O(n) method rather than
# a Diamond-level loop. StringBuilder itself stays a thin Array
# wrapper rather than its own native object: #push is already O(1)
# amortized (realloc-doubling) and #join is now O(n) total, so
# accumulating pieces in an Array and joining once at the end already
# has the right complexity -- this class just gives that pattern an
# obvious name. Diamond has no `<<` operator (see docs/syntax.md's
# operator-overloading list), so #append is a plain method, not `<<`.
class StringBuilder
  def initialize()
    @parts = []
    @total_length = 0
  end

  def append(piece)
    text = "#{piece}"
    @parts.push(text)
    @total_length = @total_length + text.length()
    self
  end

  def length() -> Int
    @total_length
  end

  def to_s() -> String
    @parts.join("")
  end
end
