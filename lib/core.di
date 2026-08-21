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
    values.last()
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
    index += 1
  end
  false
end

def array_each(values: Array, callback: Callable[1]) -> Array
  index = 0
  while index < values.length()
    callback(values[index])
    index += 1
  end
  values
end

def array_map(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index += 1
  end
  result
end

def array_map_int(values: Array, callback: Callable[1, Int]) -> Array[Int]
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index += 1
  end
  result
end

def array_map_string(values: Array, callback: Callable[1, String]) -> Array[String]
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index += 1
  end
  result
end

def array_map_typed[T, U](values: Array[T], callback: Callable[[T], U]) -> Array[U]
  result = []
  index = 0
  while index < values.length()
    result.push(callback(values[index]))
    index += 1
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
    index -= 1
  end
  result
end

def array_concat(values: Array, other: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index += 1
  end
  index = 0
  while index < other.length()
    result.push(other[index])
    index += 1
  end
  result
end

def array_compact(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    unless item == nil
      result.push(item)
    end
    index += 1
  end
  result
end

def array_uniq(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    unless result.include?(item)
      result.push(item)
    end
    index += 1
  end
  result
end

def array_flatten(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    item = values[index]
    if item is Array
      result = result.concat(item.flatten())
    else
      result.push(item)
    end
    index += 1
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
      shift += 1
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
    index += 1
  end
  values
end

def hash_keys(values: Hash) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values.key_at(index))
    index += 1
  end
  result
end

def hash_values(values: Hash) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values.value_at(index))
    index += 1
  end
  result
end

def hash_include_key(values: Hash, needle) -> Bool
  index = 0
  while index < values.length()
    if values.key_at(index) == needle
      return true
    end
    index += 1
  end
  false
end

def hash_map_values(values: Hash, callback: Callable[1]) -> Hash
  result = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    result[key] = callback(values.value_at(index))
    index += 1
  end
  result
end

def hash_merge(a: Hash, b: Hash) -> Hash
  result = {}
  index = 0
  while index < a.length()
    result[a.key_at(index)] = a.value_at(index)
    index += 1
  end
  index = 0
  while index < b.length()
    result[b.key_at(index)] = b.value_at(index)
    index += 1
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
        total += 1
      end
    end
    values.each(tally_pair)
  else
    def tally_item(item)
      if callback(item)
        total += 1
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
    index += 1
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
    index += 1
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
    index += 1
  end
  nil
end

def array_each_with_index(values: Array, callback: Callable[2]) -> Array
  index = 0
  while index < values.length()
    callback(values[index], index)
    index += 1
  end
  values
end

def enumerable_sort(values: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index += 1
  end
  i = 1
  while i < result.length()
    key = result[i]
    j = i - 1
    while j >= 0 && result[j] > key
      result[j + 1] = result[j]
      j -= 1
    end
    result[j + 1] = key
    i += 1
  end
  result
end

def enumerable_sort_by(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index])
    index += 1
  end
  i = 1
  while i < result.length()
    key = result[i]
    key_value = callback(key)
    j = i - 1
    while j >= 0 && callback(result[j]) > key_value
      result[j + 1] = result[j]
      j -= 1
    end
    result[j + 1] = key
    i += 1
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
    index += 1
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
    index += 1
  end
  result
end

def array_min_by(values: Array, callback: Callable[1])
  result = values[0]
  result_key = callback(result)
  index = 1
  while index < values.length()
    key = callback(values[index])
    if key < result_key
      result = values[index]
      result_key = key
    end
    index += 1
  end
  result
end

def array_max_by(values: Array, callback: Callable[1])
  result = values[0]
  result_key = callback(result)
  index = 1
  while index < values.length()
    key = callback(values[index])
    if key > result_key
      result = values[index]
      result_key = key
    end
    index += 1
  end
  result
end

def array_take(values: Array, n: Int) -> Array
  result = []
  index = 0
  while index < n && index < values.length()
    result.push(values[index])
    index += 1
  end
  result
end

def array_drop(values: Array, n: Int) -> Array
  result = []
  index = n
  index = 0 if index < 0
  while index < values.length()
    result.push(values[index])
    index += 1
  end
  result
end

def array_flat_map(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    mapped = callback(values[index])
    if mapped is Array
      result = result.concat(mapped)
    else
      result.push(mapped)
    end
    index += 1
  end
  result
end

# Ruby's own `partition` return shape: `[matching, non_matching]`, not a
# Hash keyed on true/false -- group_by below covers the "keyed by an
# arbitrary block result" case; partition is specifically the two-way
# split.
def array_partition(values: Array, callback: Callable[1]) -> Array
  matching = []
  non_matching = []
  index = 0
  while index < values.length()
    item = values[index]
    if callback(item)
      matching.push(item)
    else
      non_matching.push(item)
    end
    index += 1
  end
  [matching, non_matching]
end

def array_group_by(values: Array, callback: Callable[1]) -> Hash
  result = {}
  index = 0
  while index < values.length()
    item = values[index]
    key = callback(item)
    group = result[key]
    if group == nil
      result[key] = [item]
    else
      group.push(item)
    end
    index += 1
  end
  result
end

# Pads with Nil out to the *receiver's* own length, matching Ruby's own
# `[1, 2, 3].zip([4, 5])` => `[[1, 4], [2, 5], [3, nil]]` (the receiver's
# length wins, the other array is truncated or padded to match, never
# the other way around).
def array_zip(values: Array, other: Array) -> Array
  result = []
  index = 0
  while index < values.length()
    paired = if index < other.length()
      other[index]
    else
      nil
    end
    result.push([values[index], paired])
    index += 1
  end
  result
end

# Non-overlapping chunks of exactly `size`, the last one short if
# `values.length()` isn't an exact multiple -- Ruby's own `each_slice`.
# Returns the slices directly rather than taking a block: with no
# Enumerator to lazily drive a `.to_a` call, returning the collected
# Array[Array] up front is the more broadly useful shape (still
# trivially composable with `.each()`/`.map()` afterward). Assumes
# `size` is a positive Int, same as Ruby's own ArgumentError-on-`<= 0`
# contract -- not separately validated here.
def array_each_slice(values: Array, size: Int) -> Array
  result = []
  index = 0
  while index < values.length()
    slice = []
    slice_index = index
    while slice_index < values.length() && slice_index < index + size
      slice.push(values[slice_index])
      slice_index += 1
    end
    result.push(slice)
    index += size
  end
  result
end

# Overlapping (sliding-window) chunks of exactly `size` -- unlike
# each_slice, always exactly `size` long, and there are exactly
# `values.length() - size + 1` of them (none at all if the receiver is
# shorter than `size`). Ruby's own `each_cons`; same "no Enumerator, so
# return the collected windows directly" reasoning as each_slice above.
def array_each_cons(values: Array, size: Int) -> Array
  result = []
  index = 0
  while index + size <= values.length()
    window = []
    window_index = index
    while window_index < index + size
      window.push(values[window_index])
      window_index += 1
    end
    result.push(window)
    index += 1
  end
  result
end

def array_tally(values: Array) -> Hash
  result = {}
  index = 0
  while index < values.length()
    item = values[index]
    count = result[item]
    result[item] = if count == nil
      1
    else
      count + 1
    end
    index += 1
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

  # The methods above stay generic by driving self.each(...) directly, but
  # sort/min/take/etc. below are only implemented once, over Array, using
  # indexed access rather than each() -- rewriting all of them to be
  # each()-generic would duplicate already-tested Array logic. Draining
  # self into a materialized Array here and delegating to that existing
  # Array-typed implementation reuses it as-is instead.
  def to_a() -> Array
    result = []
    def collect(item)
      result.push(item)
    end
    self.each(collect)
    result
  end

  def sort() -> Array = enumerable_sort(self.to_a())
  def sort_by(callback: Callable[1]) -> Array = enumerable_sort_by(self.to_a(), callback)
  def min() = enumerable_min(self.to_a())
  def max() = enumerable_max(self.to_a())
  def min_by(callback: Callable[1]) = array_min_by(self.to_a(), callback)
  def max_by(callback: Callable[1]) = array_max_by(self.to_a(), callback)
  def reject(callback: Callable[1]) -> Array = array_reject(self.to_a(), callback)
  def find(callback: Callable[1]) = array_find(self.to_a(), callback)
  def each_with_index(callback: Callable[2]) -> Array = array_each_with_index(self.to_a(), callback)
  def sum() = array_sum(self.to_a())
  def take(n: Int) -> Array = array_take(self.to_a(), n)
  def drop(n: Int) -> Array = array_drop(self.to_a(), n)
  def flat_map(callback: Callable[1]) -> Array = array_flat_map(self.to_a(), callback)
  def partition(callback: Callable[1]) -> Array = array_partition(self.to_a(), callback)
  def group_by(callback: Callable[1]) -> Hash = array_group_by(self.to_a(), callback)
  def zip(other: Array) -> Array = array_zip(self.to_a(), other)
  def each_slice(size: Int) -> Array = array_each_slice(self.to_a(), size)
  def each_cons(size: Int) -> Array = array_each_cons(self.to_a(), size)
  def tally() -> Hash = array_tally(self.to_a())
end

# `<`/`<=`/`>`/`>=`/`==` derived from a single `<=>` an including class
# defines -- the same "several methods derived from one" relationship
# Enumerable has with `each`, just for ordering instead of iteration.
# `<=>` itself returns Nil for a genuinely incomparable pair (see
# DIAMOND_OP_COMPARE's own comment, vm.c) rather than raising, so these
# derived comparisons still end up raising on the very next operator
# (`nil < 0`) for that case -- no bespoke error handling needed here.
module Comparable
  def <(other) = (self <=> other) < 0
  def <=(other) = (self <=> other) <= 0
  def >(other) = (self <=> other) > 0
  def >=(other) = (self <=> other) >= 0
  def ==(other) = (self <=> other) == 0
  def between?(min, max) = self >= min && self <= max
  def clamp(min, max)
    return min if self < min
    return max if self > max
    self
  end
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
      i += 1
    end
    self
  end
end

