# This file is compiled as ordinary Diamond before every user program.

# first() is the first element; first(n) the first n, as an Array.
def array_first(values: Array, n = nil)
  return values[0] if n == nil
  values.take(n)
end

def array_first_or(values: Array, fallback)
  if values.length() == 0
    fallback
  else
    values[0]
  end
end

def array_last(values: Array, n = nil)
  return values[values.length() - 1] if n == nil
  return [] if n <= 0
  values.drop(max(values.length() - n, 0))
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

# The position of the first element == needle, or nil -- the Array
# counterpart of String#index_of.
def array_index_of(values: Array, needle) -> Int | Nil
  index = 0
  while index < values.length()
    return index if values[index] == needle
    index += 1
  end
  nil
end

def array_each(values: Array, callback: Callable[1]) -> Array
  index = 0
  while index < values.length()
    callback(values[index])
    index += 1
  end
  values
end

def array_each_until(values: Array, callback: Callable[1, Bool]) -> Array
  index = 0
  continuing = true
  while index < values.length() && continuing
    continuing = callback(values[index])
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

# Scalars are remembered in a Hash, whose key equality matches == for
# them (1 and 1.0 are one key), so the common case is linear. Anything
# else -- an instance may define its own ==, which Hash keys don't use --
# is compared against the kept elements with ==.
def array_uniq(values: Array) -> Array
  result = []
  seen = {}
  others = []
  index = 0
  while index < values.length()
    item = values[index]
    if item == nil || item is Bool || item is Int || item is Float ||
       item is String || item is Symbol
      unless seen.include_key?(item)
        seen[item] = true
        result.push(item)
      end
    elsif !others.include?(item)
      others.push(item)
      result.push(item)
    end
    index += 1
  end
  result
end

# Appends `values`, recursively flattened, onto `result` -- in place, so
# flattening is linear rather than re-copying the result per nested Array.
# Defined above array_flatten: selfhost/parser.di needs a def before its
# first call.
def diamond_flatten_into(values: Array, result: Array)
  index = 0
  while index < values.length()
    item = values[index]
    if item is Array
      diamond_flatten_into(item, result)
    else
      result.push(item)
    end
    index += 1
  end
end

def array_flatten(values: Array) -> Array
  result = []
  diamond_flatten_into(values, result)
  result
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

# The free-function spelling of the native Hash#include_key?.
def hash_include_key(values: Hash, needle) -> Bool = values.include_key?(needle)

# The value for `key`, or `fallback` only when the key is absent -- a key
# present with a nil value returns nil. include_key? is a native hash
# lookup, so this is O(1).
# fetch(key) with no fallback raises IndexError for a missing key (Ruby
# raises KeyError, a kind of IndexError).
def hash_fetch(values: Hash, key, fallback = :__diamond_no_fallback__)
  return values[key] if values.include_key?(key)
  raise IndexError.new("key not found: #{key}") if fallback == :__diamond_no_fallback__
  fallback
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

def hash_each_until(values: Hash, callback: Callable[1, Bool]) -> Hash
  index = 0
  continuing = true
  while index < values.length() && continuing
    continuing = callback(values.value_at(index))
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

# sum() adds the elements; with a block, it adds what the block returns
# for each one.
def array_sum(values: Array, callback = nil)
  total = 0
  index = 0
  while index < values.length()
    total = total + (if callback == nil then values[index] else callback(values[index]) end)
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

# Ordering support for sort, sort_by, min, max, min_by, and max_by. Two
# Arrays compare element by element, then by length, like Ruby's Array#<=>,
# so [count, name] pairs work as sort keys. Anything else uses the same
# operator each method always used (> for sort and max, < for min), so a
# class defining only that one operator keeps working. These are top-level
# functions rather than a module because they recurse, and the diamond_
# prefix isn't a collection-bridge prefix, so none becomes a method.
def diamond_sort_greater(a, b) -> Bool
  return a > b unless a is Array && b is Array
  index = 0
  while index < a.length() && index < b.length()
    return true if diamond_sort_greater(a[index], b[index])
    return false if diamond_sort_greater(b[index], a[index])
    index += 1
  end
  a.length() > b.length()
end

def diamond_sort_less(a, b) -> Bool
  return a < b unless a is Array && b is Array
  index = 0
  while index < a.length() && index < b.length()
    return true if diamond_sort_less(a[index], b[index])
    return false if diamond_sort_less(b[index], a[index])
    index += 1
  end
  a.length() < b.length()
end

# The stable ascending order of `keys`, as indices. A bottom-up merge
# sort: O(n log n) comparisons, and iterative, since Diamond's call depth
# is bounded.
def diamond_sort_order(keys: Array) -> Array
  count = keys.length()
  order = []
  index = 0
  while index < count
    order.push(index)
    index += 1
  end
  width = 1
  while width < count
    merged = []
    start = 0
    while start < count
      # Not min(): a program may define its own function by that name.
      middle = if start + width < count then start + width else count end
      stop = if start + width * 2 < count then start + width * 2 else count end
      left = start
      right = middle
      while left < middle && right < stop
        # Take from the right only when strictly smaller, so equal keys
        # keep their original order.
        if diamond_sort_greater(keys[order[left]], keys[order[right]])
          merged.push(order[right])
          right += 1
        else
          merged.push(order[left])
          left += 1
        end
      end
      while left < middle
        merged.push(order[left])
        left += 1
      end
      while right < stop
        merged.push(order[right])
        right += 1
      end
      start = stop
    end
    order = merged
    width *= 2
  end
  order
end

def diamond_sort_pick(values: Array, order: Array) -> Array
  result = []
  index = 0
  while index < order.length()
    result.push(values[order[index]])
    index += 1
  end
  result
end

def enumerable_sort(values: Array) -> Array
  diamond_sort_pick(values, diamond_sort_order(values))
end

# Calls the block once per element, not once per comparison.
def enumerable_sort_by(values: Array, callback: Callable[1]) -> Array
  keys = []
  index = 0
  while index < values.length()
    keys.push(callback(values[index]))
    index += 1
  end
  diamond_sort_pick(values, diamond_sort_order(keys))
end

def enumerable_min(values: Array)
  result = values[0]
  index = 1
  while index < values.length()
    if diamond_sort_less(values[index], result)
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
    if diamond_sort_greater(values[index], result)
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
    if diamond_sort_less(key, result_key)
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
    if diamond_sort_greater(key, result_key)
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
      # Append in place: result.concat(mapped) would copy all of result
      # again for every element, making flat_map quadratic.
      inner = 0
      while inner < mapped.length()
        result.push(mapped[inner])
        inner += 1
      end
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

# A deliberately small, composable lazy pipeline. Declared before Enumerable
# so the deferred self-hosted frontend does not need forward class resolution.
class LazyEnumerator
  def initialize(source, operations: Array)
    @source = source
    @operations = operations
  end

  def append(kind: Symbol, callback: Callable[1])
    operations = @operations.dup()
    operations.push([kind, callback])
    LazyEnumerator.new(@source, operations)
  end

  def map(callback: Callable[1]) = self.append(:map, callback)
  def select(callback: Callable[1]) = self.append(:select, callback)
  def reject(callback: Callable[1]) = self.append(:reject, callback)

  def each_until(callback: Callable[1, Bool])
    operations = @operations
    def process(source_value) -> Bool
      value = source_value
      accepted = true
      index = 0
      while index < operations.length() && accepted
        operation = operations[index]
        kind = operation[0]
        transform = operation[1]
        if kind == :map
          value = transform(value)
        elsif kind == :select
          accepted = false unless transform(value)
        else
          accepted = false if transform(value)
        end
        index += 1
      end
      if accepted
        callback(value)
      else
        true
      end
    end
    if @source is Array
      array_each_until(@source, process)
    elsif @source is Hash
      hash_each_until(@source, process)
    else
      @source.each_until(process)
    end
    self
  end

  def each(callback: Callable[1])
    def continue_each(value) -> Bool
      callback(value)
      true
    end
    self.each_until(continue_each)
  end

  def take(count: Int) -> Array
    result = []
    if count > 0
      def take_value(value) -> Bool
        result.push(value)
        result.length() < count
      end
      self.each_until(take_value)
    end
    result
  end

  def find(callback: Callable[1])
    result = nil
    def find_value(value) -> Bool
      if callback(value)
        result = value
        false
      else
        true
      end
    end
    self.each_until(find_value)
    result
  end

  def any?(callback: Callable[1]) -> Bool
    found = false
    def any_value(value) -> Bool
      found = callback(value)
      !found
    end
    self.each_until(any_value)
    found
  end

  def all?(callback: Callable[1]) -> Bool
    matched = true
    def all_value(value) -> Bool
      matched = callback(value)
      matched
    end
    self.each_until(all_value)
    matched
  end

  def to_a() -> Array
    result = []
    def collect(item)
      result.push(item)
    end
    self.each(collect)
    result
  end

  def force() -> Array = self.to_a()
end

def enumerable_lazy(value) = LazyEnumerator.new(value, [])

module Enumerable
  def lazy() = LazyEnumerator.new(self, [])
  def select(callback: Callable[1]) -> Array = enumerable_select(self, callback)
  def count(callback: Callable[1]) -> Int = enumerable_count(self, callback)
  def any?(callback: Callable[1]) -> Bool = enumerable_any(self, callback)
  def all?(callback: Callable[1]) -> Bool = enumerable_all(self, callback)
  def each_until(callback: Callable[1, Bool])
    continuing = true
    def visit(value)
      continuing = callback(value) if continuing
    end
    self.each(visit)
    self
  end
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

  # first() is the start; first(n) the first n values, as an Array.
  def first(n = nil)
    return @start if n == nil
    self.to_a().take(n)
  end
  def last() -> Int = @end

  # Every `size`-th value from the start: (1..10).step(3) is [1, 4, 7, 10].
  def step(size: Int) -> Array[Int]
    raise ArgumentError.new("step must be positive") if size <= 0
    result = []
    i = @start
    stop = if @exclusive then @end else @end + 1 end
    while i < stop
      result.push(i)
      i += size
    end
    result
  end

  def reverse_each(callback: Callable[1])
    self.to_a().reverse().each(callback)
    self
  end
  def exclusive?() -> Bool = @exclusive

  def to_s() -> String
    dots = if @exclusive then "..." else ".." end
    "#{@start}#{dots}#{@end}"
  end

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


  def each_until(callback: Callable[1, Bool])
    i = @start
    stop = if @exclusive
      @end
    else
      @end + 1
    end
    continuing = true
    while i < stop && continuing
      continuing = callback(i)
      i += 1
    end
    self
  end
end

# --- Ruby-compatible methods on built-in values ------------------------
# Reached through the extension protocol (find_collection_extension and
# find_value_extension, src/vm.c): `xs.foo(args)` on an Array with no
# native foo runs a program's own array_foo(xs, args) if there is one, and
# otherwise diamond_array_foo below; likewise hash_ for a Hash, string_
# for a String, integer_/float_ then numeric_ for numbers. A trailing `?`
# is dropped from the name. These all carry the diamond_ prefix so they
# can't collide with a program's own function names. Operators on an
# Array or String that the VM doesn't handle natively call
# diamond_array_op_/diamond_string_op_ functions.

def diamond_array_size(values: Array) -> Int = values.length()
def diamond_array_to_a(values: Array) -> Array = values

# Inserts `item` before position `index` (counting from the end when
# negative, where -1 appends), in place, and returns the array.
def diamond_array_insert(values: Array, index: Int, item) -> Array
  length = values.length()
  at = if index < 0 then index + length + 1 else index end
  raise IndexError.new("index #{index} out of bounds for insert into Array of length #{length}") if at < 0 || at > length
  tail = []
  while values.length() > at
    tail.push(values.pop())
  end
  values.push(item)
  while tail.length() > 0
    values.push(tail.pop())
  end
  values
end

def diamond_array_unshift(values: Array, item) -> Array = diamond_array_insert(values, 0, item)

# Removes and returns the first element, or nil when empty.
def diamond_array_shift(values: Array)
  return nil if values.length() == 0
  values.delete_at(0)
end

def diamond_array_clear(values: Array) -> Array
  while values.length() > 0
    values.pop()
  end
  values
end

# Replaces the contents with `other`'s, in place.
def diamond_array_replace(values: Array, other: Array) -> Array
  copy = other.dup()
  diamond_array_clear(values)
  index = 0
  while index < copy.length()
    values.push(copy[index])
    index += 1
  end
  values
end

# count() is the length; count(value) counts elements == value; with a
# block, it counts elements the block accepts.
def diamond_array_count(values: Array, target = :__diamond_absent__) -> Int
  return values.length() if target == :__diamond_absent__
  total = 0
  index = 0
  while index < values.length()
    item = values[index]
    matched = if target is Callable then target(item) else item == target end
    total += 1 if matched
    index += 1
  end
  total
end

# The position of the first element == target, or the first one the
# block accepts; nil if there's none.
def diamond_array_find_index(values: Array, target) -> Int | Nil
  index = 0
  while index < values.length()
    item = values[index]
    matched = if target is Callable then target(item) else item == target end
    return index if matched
    index += 1
  end
  nil
end

def diamond_array_index(values: Array, target) -> Int | Nil = diamond_array_find_index(values, target)

def diamond_array_rindex(values: Array, target) -> Int | Nil
  index = values.length() - 1
  while index >= 0
    item = values[index]
    matched = if target is Callable then target(item) else item == target end
    return index if matched
    index -= 1
  end
  nil
end

# Removes every element == target, in place. Returns target, or nil if
# none was there.
def diamond_array_delete(values: Array, target)
  kept = []
  found = false
  index = 0
  while index < values.length()
    if values[index] == target
      found = true
    else
      kept.push(values[index])
    end
    index += 1
  end
  diamond_array_replace(values, kept)
  if found then target else nil end
end

def diamond_array_delete_if(values: Array, callback: Callable[1]) -> Array
  diamond_array_replace(values, values.reject(callback))
end

def diamond_array_keep_if(values: Array, callback: Callable[1]) -> Array
  diamond_array_replace(values, values.select(callback))
end

def diamond_array_fill(values: Array, item) -> Array
  index = 0
  while index < values.length()
    values[index] = item
    index += 1
  end
  values
end

# values_at, dig, slice, and except take up to six arguments (the
# self-hosted parser, which reads this file too, has no *rest parameters).
def diamond_present_arguments(a, b, c, d, e, f) -> Array
  result = []
  result.push(a) unless a == :__diamond_absent__
  result.push(b) unless b == :__diamond_absent__
  result.push(c) unless c == :__diamond_absent__
  result.push(d) unless d == :__diamond_absent__
  result.push(e) unless e == :__diamond_absent__
  result.push(f) unless f == :__diamond_absent__
  result
end

def diamond_array_values_at(values: Array, a = :__diamond_absent__, b = :__diamond_absent__, c = :__diamond_absent__, d = :__diamond_absent__, e = :__diamond_absent__, f = :__diamond_absent__) -> Array
  indexes = diamond_present_arguments(a, b, c, d, e, f)
  result = []
  index = 0
  while index < indexes.length()
    result.push(values[indexes[index]])
    index += 1
  end
  result
end

# dig(i, j, ...) follows each index or key in turn, stopping at nil.
def diamond_array_dig(values: Array, a = :__diamond_absent__, b = :__diamond_absent__, c = :__diamond_absent__, d = :__diamond_absent__, e = :__diamond_absent__, f = :__diamond_absent__)
  path = diamond_present_arguments(a, b, c, d, e, f)
  current = values
  index = 0
  while index < path.length()
    return nil if current == nil
    current = current[path[index]]
    index += 1
  end
  current
end

def diamond_hash_dig(values: Hash, a = :__diamond_absent__, b = :__diamond_absent__, c = :__diamond_absent__, d = :__diamond_absent__, e = :__diamond_absent__, f = :__diamond_absent__)
  path = diamond_present_arguments(a, b, c, d, e, f)
  current = values
  index = 0
  while index < path.length()
    return nil if current == nil
    current = current[path[index]]
    index += 1
  end
  current
end

# [[key, value], ...] to a Hash.
def diamond_array_to_h(values: Array) -> Hash
  result = {}
  index = 0
  while index < values.length()
    pair = values[index]
    unless pair is Array && pair.length() == 2
      raise TypeError.new("to_h needs [key, value] pairs, got #{pair} at #{index}")
    end
    result[pair[0]] = pair[1]
    index += 1
  end
  result
end

def diamond_array_transpose(values: Array) -> Array
  return [] if values.length() == 0
  width = values[0].length()
  row_index = 0
  while row_index < values.length()
    raise IndexError.new("transpose needs rows of equal length") unless values[row_index].length() == width
    row_index += 1
  end
  result = []
  column = 0
  while column < width
    result.push([])
    row_index = 0
    while row_index < values.length()
      row = values[row_index]
      result[column].push(row[column])
      row_index += 1
    end
    column += 1
  end
  result
end

def diamond_array_minmax(values: Array) -> Array = [values.min(), values.max()]

def diamond_array_filter_map(values: Array, callback: Callable[1]) -> Array
  result = []
  index = 0
  while index < values.length()
    mapped = callback(values[index])
    result.push(mapped) unless mapped == nil || mapped == false
    index += 1
  end
  result
end

def diamond_array_each_with_object(values: Array, memo, callback: Callable[2])
  index = 0
  while index < values.length()
    callback(values[index], memo)
    index += 1
  end
  memo
end

def diamond_array_none(values: Array, callback: Callable[1]) -> Bool = !values.any?(callback)

def diamond_array_one(values: Array, callback: Callable[1]) -> Bool = values.count(callback) == 1

def diamond_array_inject(values: Array, initial, callback: Callable[2]) = values.reduce(initial, callback)

def diamond_array_sum_by(values: Array, callback: Callable[1]) = array_sum(values, callback)

def diamond_array_product(values: Array, other: Array) -> Array
  result = []
  left = 0
  while left < values.length()
    right = 0
    while right < other.length()
      result.push([values[left], other[right]])
      right += 1
    end
    left += 1
  end
  result
end

# Every k-element combination, in order.
def diamond_array_combination(values: Array, k: Int) -> Array
  return [[]] if k == 0
  return [] if k > values.length() || k < 0
  result = []
  index = 0
  while index <= values.length() - k
    first = values[index]
    tails = diamond_array_combination(values.drop(index + 1), k - 1)
    tail = 0
    while tail < tails.length()
      result.push([first] + tails[tail])
      tail += 1
    end
    index += 1
  end
  result
end

# Groups runs of neighbours the block says belong together.
def diamond_array_chunk_while(values: Array, callback: Callable[2]) -> Array
  return [] if values.length() == 0
  groups = [[values[0]]]
  index = 1
  while index < values.length()
    if callback(values[index - 1], values[index])
      groups[groups.length() - 1].push(values[index])
    else
      groups.push([values[index]])
    end
    index += 1
  end
  groups
end

def diamond_array_rotate(values: Array, count: Int = 1) -> Array
  return [] if values.length() == 0
  shift = count % values.length()
  values.drop(shift) + values.take(shift)
end

# Array - Array: left's elements that aren't in right. & is the elements
# in both, | the union; each keeps first-seen order without duplicates.
def diamond_array_op_minus(values: Array, other) -> Array
  raise TypeError.new("Array - needs an Array, got #{other}") unless other is Array
  result = []
  index = 0
  while index < values.length()
    result.push(values[index]) unless other.include?(values[index])
    index += 1
  end
  result
end

def diamond_array_op_and(values: Array, other) -> Array
  raise TypeError.new("Array & needs an Array, got #{other}") unless other is Array
  unique = values.uniq()
  result = []
  index = 0
  while index < unique.length()
    result.push(unique[index]) if other.include?(unique[index])
    index += 1
  end
  result
end

def diamond_array_op_or(values: Array, other) -> Array
  raise TypeError.new("Array | needs an Array, got #{other}") unless other is Array
  (values + other).uniq()
end

# Array * n repeats it n times; Array * separator joins it.
def diamond_array_op_times(values: Array, times)
  return values.join(times) if times is String
  raise TypeError.new("Array * needs an Int or a String, got #{times}") unless times is Int
  raise ArgumentError.new("negative argument to Array *") if times < 0
  # Appended in place: `result = result + values` copies everything so far
  # (twice) on each pass, which made `[0] * n` quadratic -- minutes for a
  # few hundred thousand elements.
  result = []
  count = 0
  while count < times
    index = 0
    while index < values.length()
      result.push(values[index])
      index += 1
    end
    count += 1
  end
  result
end

# --- Hash -----------------------------------------------------------------
# A block taking two parameters gets (key, value), as in Ruby; one taking a
# single parameter gets the value, as Diamond's Hash iteration always has.
def diamond_hash_call_pair(callback, key, value)
  if callback.arity() == 2 then callback(key, value) else callback(value) end
end

def diamond_hash_size(values: Hash) -> Int = values.length()
def diamond_hash_to_h(values: Hash) -> Hash = values
def diamond_hash_key(values: Hash, key) -> Bool = values.include_key?(key)
def diamond_hash_has_key(values: Hash, key) -> Bool = values.include_key?(key)
def diamond_hash_include(values: Hash, key) -> Bool = values.include_key?(key)
def diamond_hash_member(values: Hash, key) -> Bool = values.include_key?(key)

def diamond_hash_value(values: Hash, target) -> Bool = values.values().include?(target)
def diamond_hash_has_value(values: Hash, target) -> Bool = values.values().include?(target)

def diamond_hash_to_a(values: Hash) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push([values.key_at(index), values.value_at(index)])
    index += 1
  end
  result
end

def diamond_hash_map(values: Hash, callback) -> Array
  result = []
  index = 0
  while index < values.length()
    result.push(diamond_hash_call_pair(callback, values.key_at(index), values.value_at(index)))
    index += 1
  end
  result
end

# With a (key, value) block, select/reject/filter keep the matching
# entries as a Hash. select with a one-parameter block keeps its old
# meaning, the matching values as an Array.
def diamond_hash_select(values: Hash, callback)
  return enumerable_select(values, callback) unless callback.arity() == 2
  result = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    result[key] = values.value_at(index) if callback(key, values.value_at(index))
    index += 1
  end
  result
end

def diamond_hash_filter(values: Hash, callback) = diamond_hash_select(values, callback)

def diamond_hash_reject(values: Hash, callback) -> Hash
  result = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    result[key] = values.value_at(index) unless diamond_hash_call_pair(callback, key, values.value_at(index))
    index += 1
  end
  result
end

def diamond_hash_any(values: Hash, callback) -> Bool
  index = 0
  while index < values.length()
    return true if diamond_hash_call_pair(callback, values.key_at(index), values.value_at(index))
    index += 1
  end
  false
end

def diamond_hash_all(values: Hash, callback) -> Bool
  index = 0
  while index < values.length()
    return false unless diamond_hash_call_pair(callback, values.key_at(index), values.value_at(index))
    index += 1
  end
  true
end

def diamond_hash_none(values: Hash, callback) -> Bool = !diamond_hash_any(values, callback)

# count() is the number of entries; with a block, the entries it accepts.
def diamond_hash_count(values: Hash, callback = nil) -> Int
  return values.length() if callback == nil
  total = 0
  index = 0
  while index < values.length()
    total += 1 if diamond_hash_call_pair(callback, values.key_at(index), values.value_at(index))
    index += 1
  end
  total
end

# The first [key, value] entry the block accepts, or nil.
def diamond_hash_find(values: Hash, callback)
  index = 0
  while index < values.length()
    key = values.key_at(index)
    return [key, values.value_at(index)] if diamond_hash_call_pair(callback, key, values.value_at(index))
    index += 1
  end
  nil
end

# sum() adds the values; with a block, what the block returns for each
# entry.
def diamond_hash_sum(values: Hash, callback = nil)
  total = 0
  index = 0
  while index < values.length()
    total = total + (if callback == nil then values.value_at(index) else diamond_hash_call_pair(callback, values.key_at(index), values.value_at(index)) end)
    index += 1
  end
  total
end

# min_by/max_by/sort_by work on [key, value] entries, as in Ruby.
def diamond_hash_sort_by(values: Hash, callback) -> Array
  def pair_key(pair)
    diamond_hash_call_pair(callback, pair[0], pair[1])
  end
  diamond_hash_to_a(values).sort_by(pair_key)
end

def diamond_hash_min_by(values: Hash, callback)
  def pair_key(pair)
    diamond_hash_call_pair(callback, pair[0], pair[1])
  end
  diamond_hash_to_a(values).min_by(pair_key)
end

def diamond_hash_max_by(values: Hash, callback)
  def pair_key(pair)
    diamond_hash_call_pair(callback, pair[0], pair[1])
  end
  diamond_hash_to_a(values).max_by(pair_key)
end

def diamond_hash_sort(values: Hash) -> Array
  def entry_key(pair)
    pair[0]
  end
  diamond_hash_to_a(values).sort_by(entry_key)
end

def diamond_hash_group_by(values: Hash, callback) -> Hash
  groups = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    group = diamond_hash_call_pair(callback, key, values.value_at(index))
    groups[group] = {} unless groups.include_key?(group)
    bucket = groups[group]
    bucket[key] = values.value_at(index)
    index += 1
  end
  groups
end

def diamond_hash_partition(values: Hash, callback) -> Array
  accepted = {}
  rejected = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    if diamond_hash_call_pair(callback, key, values.value_at(index))
      accepted[key] = values.value_at(index)
    else
      rejected[key] = values.value_at(index)
    end
    index += 1
  end
  [accepted, rejected]
end

def diamond_hash_filter_map(values: Hash, callback) -> Array
  result = []
  index = 0
  while index < values.length()
    mapped = diamond_hash_call_pair(callback, values.key_at(index), values.value_at(index))
    result.push(mapped) unless mapped == nil || mapped == false
    index += 1
  end
  result
end

# The block gets ([key, value], memo) and the memo is returned.
def diamond_hash_each_with_object(values: Hash, memo, callback: Callable[2])
  index = 0
  while index < values.length()
    callback([values.key_at(index), values.value_at(index)], memo)
    index += 1
  end
  memo
end

def diamond_hash_each_with_index(values: Hash, callback: Callable[2]) -> Hash
  index = 0
  while index < values.length()
    callback([values.key_at(index), values.value_at(index)], index)
    index += 1
  end
  values
end

def diamond_hash_each_pair(values: Hash, callback: Callable[2]) -> Hash = values.each(callback)

def diamond_hash_transform_values(values: Hash, callback: Callable[1]) -> Hash = values.map_values(callback)

def diamond_hash_transform_keys(values: Hash, callback: Callable[1]) -> Hash
  result = {}
  index = 0
  while index < values.length()
    result[callback(values.key_at(index))] = values.value_at(index)
    index += 1
  end
  result
end

def diamond_hash_invert(values: Hash) -> Hash
  result = {}
  index = 0
  while index < values.length()
    result[values.value_at(index)] = values.key_at(index)
    index += 1
  end
  result
end

# Merges `other` into this Hash in place.
def diamond_hash_update(values: Hash, other: Hash) -> Hash
  index = 0
  while index < other.length()
    values[other.key_at(index)] = other.value_at(index)
    index += 1
  end
  values
end

def diamond_hash_slice(values: Hash, a = :__diamond_absent__, b = :__diamond_absent__, c = :__diamond_absent__, d = :__diamond_absent__, e = :__diamond_absent__, f = :__diamond_absent__) -> Hash
  keys = diamond_present_arguments(a, b, c, d, e, f)
  result = {}
  index = 0
  while index < keys.length()
    result[keys[index]] = values[keys[index]] if values.include_key?(keys[index])
    index += 1
  end
  result
end

def diamond_hash_except(values: Hash, a = :__diamond_absent__, b = :__diamond_absent__, c = :__diamond_absent__, d = :__diamond_absent__, e = :__diamond_absent__, f = :__diamond_absent__) -> Hash
  keys = diamond_present_arguments(a, b, c, d, e, f)
  result = {}
  index = 0
  while index < values.length()
    key = values.key_at(index)
    result[key] = values.value_at(index) unless keys.include?(key)
    index += 1
  end
  result
end

def diamond_hash_compact(values: Hash) -> Hash
  def value_is_nil(value)
    value == nil
  end
  diamond_hash_reject(values, value_is_nil)
end

# --- String ---------------------------------------------------------------

def diamond_string_size(text: String) -> Int = text.length()
def diamond_string_bytesize(text: String) -> Int = text.length()
def diamond_string_index(text: String, needle: String) -> Int | Nil = text.index_of(needle)
def diamond_string_hex(text: String) -> Int = text.to_i(16)
def diamond_string_oct(text: String) -> Int = text.to_i(8)
def diamond_string_to_sym(text: String) -> Symbol = to_sym(text)
def diamond_string_match(text: String, pattern) -> Bool = pattern.match?(text)
def diamond_string_to_s(text: String) -> String = text

# The last position of `needle`, or nil.
def diamond_string_rindex(text: String, needle: String) -> Int | Nil
  position = text.length() - needle.length()
  while position >= 0
    return position if text.slice(position, needle.length()) == needle
    position -= 1
  end
  nil
end

def diamond_string_swapcase(text: String) -> String
  chars = text.chars()
  result = []
  index = 0
  while index < chars.length()
    c = chars[index]
    result.push(if c.upcase() == c then c.downcase() else c.upcase() end)
    index += 1
  end
  result.join("")
end

# count(set) counts the characters that appear in `set`.
def diamond_string_count(text: String, set: String) -> Int
  chars = text.chars()
  total = 0
  index = 0
  while index < chars.length()
    total += 1 if set.include?(chars[index])
    index += 1
  end
  total
end

# delete(set) removes every character that appears in `set`.
def diamond_string_delete(text: String, set: String) -> String
  chars = text.chars()
  result = []
  index = 0
  while index < chars.length()
    result.push(chars[index]) unless set.include?(chars[index])
    index += 1
  end
  result.join("")
end

# squeeze() collapses each run of a repeated character to one.
def diamond_string_squeeze(text: String) -> String
  chars = text.chars()
  result = []
  index = 0
  while index < chars.length()
    c = chars[index]
    result.push(c) unless result.length() > 0 && result[result.length() - 1] == c
    index += 1
  end
  result.join("")
end

# The lines, each keeping its "\n" (the last one may not have one).
def diamond_string_lines(text: String) -> Array[String]
  result = []
  start = 0
  index = 0
  while index < text.length()
    if text.getbyte(index) == 10
      result.push(text.slice(start, index - start + 1))
      start = index + 1
    end
    index += 1
  end
  result.push(text.slice(start, text.length() - start)) if start < text.length()
  result
end

def diamond_string_each_line(text: String, callback: Callable[1]) -> String
  diamond_string_lines(text).each(callback)
  text
end

def diamond_string_each_char(text: String, callback: Callable[1]) -> String
  text.chars().each(callback)
  text
end

def diamond_string_each_byte(text: String, callback: Callable[1]) -> String
  index = 0
  while index < text.length()
    callback(text.getbyte(index))
    index += 1
  end
  text
end

def diamond_string_center(text: String, width: Int, padding: String = " ") -> String
  total = width - text.length()
  return text if total <= 0
  left = total / 2
  text.rjust(text.length() + left, padding).ljust(width, padding)
end

def diamond_string_chop(text: String) -> String
  return "" if text.length() == 0
  return text.slice(0, text.length() - 2) if text.end_with?("\r\n")
  text.slice(0, text.length() - 1)
end

def diamond_string_delete_prefix(text: String, prefix: String) -> String
  if text.start_with?(prefix) then text.slice(prefix.length(), text.length() - prefix.length()) else text end
end

def diamond_string_delete_suffix(text: String, suffix: String) -> String
  if text.end_with?(suffix) then text.slice(0, text.length() - suffix.length()) else text end
end

# [before, separator, after] around the first `separator`, or
# [text, "", ""] when it isn't there.
def diamond_string_partition(text: String, separator: String) -> Array[String]
  at = text.index_of(separator)
  return [text, "", ""] if at == nil
  after = at + separator.length()
  [text.slice(0, at), separator, text.slice(after, text.length() - after)]
end

def diamond_string_casecmp(text: String, other: String) -> Bool = text.downcase() == other.downcase()

def diamond_string_between(text: String, low: String, high: String) -> Bool = text >= low && text <= high

def diamond_string_op_times(text: String, times) -> String
  raise TypeError.new("String * needs an Int, got #{times}") unless times is Int
  raise ArgumentError.new("negative argument to String *") if times < 0
  text.repeat(times)
end

# --- Int and Float --------------------------------------------------------

# base ** exponent: exact for an Int base and a non-negative Int exponent
# (growing past 64 bits as needed), a Float otherwise.
def diamond_numeric_pow(base, exponent)
  if base is Int && exponent is Int && exponent >= 0
    result = 1
    factor = base
    remaining = exponent
    while remaining > 0
      result = result * factor if remaining % 2 == 1
      remaining = remaining / 2
      factor = factor * factor if remaining > 0
    end
    result
  else
    pow(base.to_f(), exponent.to_f())
  end
end

def diamond_numeric_clamp(value, low, high)
  return low if value < low
  return high if value > high
  value
end

def diamond_numeric_between(value, low, high) -> Bool = value >= low && value <= high
def diamond_numeric_zero(value) -> Bool = value == 0
def diamond_numeric_positive(value) -> Bool = value > 0
def diamond_numeric_negative(value) -> Bool = value < 0
def diamond_numeric_fdiv(value, other) -> Float = value.to_f() / other.to_f()

# [quotient, remainder], rounding the quotient down as Ruby does.
def diamond_numeric_divmod(value, other) -> Array
  quotient = (value.to_f() / other.to_f()).floor()
  [quotient, value - quotient * other]
end

def diamond_integer_even(value: Int) -> Bool = value % 2 == 0
def diamond_integer_odd(value: Int) -> Bool = value % 2 != 0
def diamond_integer_succ(value: Int) -> Int = value + 1
def diamond_integer_next(value: Int) -> Int = value + 1
def diamond_integer_pred(value: Int) -> Int = value - 1

def diamond_integer_gcd(value: Int, other: Int) -> Int
  a = value.abs()
  b = other.abs()
  while b != 0
    held = b
    b = a % b
    a = held
  end
  a
end

def diamond_integer_lcm(value: Int, other: Int) -> Int
  return 0 if value == 0 || other == 0
  (value / diamond_integer_gcd(value, other) * other).abs()
end

# The digits, least significant first, as in Ruby.
def diamond_integer_digits(value: Int, base: Int = 10) -> Array[Int]
  raise ArgumentError.new("digits of a negative number") if value < 0
  return [0] if value == 0
  result = []
  remaining = value
  while remaining > 0
    result.push(remaining % base)
    remaining = remaining / base
  end
  result
end

def diamond_float_truncate(value: Float) -> Int = value.to_i()
def diamond_float_nan(value: Float) -> Bool = value != value
def diamond_float_infinite(value: Float) -> Bool = value == value && (value - value) != 0.0
def diamond_float_finite(value: Float) -> Bool = value - value == 0.0

# --- Math -------------------------------------------------------------------
# The math builtins (sqrt, sin, pow, ...) are also plain functions; Math
# gives them their Ruby names.
def diamond_math_sqrt(x) = sqrt(x)
def diamond_math_sin(x) = sin(x)
def diamond_math_cos(x) = cos(x)
def diamond_math_tan(x) = tan(x)
def diamond_math_exp(x) = exp(x)
def diamond_math_log(x) = log(x)
def diamond_math_tanh(x) = tanh(x)
def diamond_math_pow(x, y) = pow(x, y)

module Math
  def self.sqrt(x) -> Float = diamond_math_sqrt(x)
  def self.cbrt(x) -> Float = diamond_math_pow(x.to_f(), 1.0 / 3.0)
  def self.sin(x) -> Float = diamond_math_sin(x)
  def self.cos(x) -> Float = diamond_math_cos(x)
  def self.tan(x) -> Float = diamond_math_tan(x)
  def self.exp(x) -> Float = diamond_math_exp(x)
  def self.log(x) -> Float = diamond_math_log(x)
  def self.log2(x) -> Float = diamond_math_log(x) / diamond_math_log(2.0)
  def self.log10(x) -> Float = diamond_math_log(x) / diamond_math_log(10.0)
  def self.tanh(x) -> Float = diamond_math_tanh(x)
  def self.hypot(x, y) -> Float = diamond_math_sqrt(x * x + y * y)
  def self.pi() -> Float = 3.141592653589793
  def self.e() -> Float = 2.718281828459045
end

# split() with no separator splits on runs of whitespace, ignoring any at
# either end. split(separator, limit) with a positive limit makes at most
# `limit` pieces, the last holding the rest of the text.
def diamond_string_split_extended(text: String, separator = nil, limit: Int = 0) -> Array[String]
  if separator == nil
    stripped = text.strip()
    return [] if stripped.empty?()
    pieces = stripped.split(Regexp.new("\\s+"))
    return pieces if limit <= 0 || pieces.length() <= limit
    return pieces.take(limit - 1) + [pieces.drop(limit - 1).join(" ")]
  end
  return text.split(separator) if limit <= 0
  raise TypeError.new("split with a limit needs a String separator") unless separator is String
  pieces = []
  rest = text
  while pieces.length() < limit - 1
    at = rest.index_of(separator)
    break if at == nil
    pieces.push(rest.slice(0, at))
    rest = rest.slice(at + separator.length(), rest.length() - at - separator.length())
  end
  pieces.push(rest)
  pieces
end
