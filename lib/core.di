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

module Enumerable
  def select(callback: Callable[1]) -> Array = enumerable_select(self, callback)
  def count(callback: Callable[1]) -> Int = enumerable_count(self, callback)
  def any?(callback: Callable[1]) -> Bool = enumerable_any(self, callback)
  def all?(callback: Callable[1]) -> Bool = enumerable_all(self, callback)
  def map(callback: Callable[1]) -> Array = enumerable_map(self, callback)
  def reduce(initial, callback: Callable[2]) = enumerable_reduce(self, initial, callback)
end
